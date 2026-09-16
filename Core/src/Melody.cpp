/**
 * @file Melody.cpp
 * @brief Chords, acid patterns, lead phrases, arps and their schedule.
 */
#include "phos/Melody.h"
#include "phos/Corpus.h"
#include "phos/Model.h"
#include "phos/Dsp.h"
#include "phos/Harmony.h"
#include "phos/Rhythm.h"
#include "phos/Sfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace phos {

namespace {

constexpr uint64_t kSaltChords = 0x43484F5244000001ull;
constexpr uint64_t kSaltAcid   = 0x4143494400000002ull;
constexpr uint64_t kSaltLead   = 0x4C45414400000003ull;
constexpr uint64_t kSaltArp    = 0x4152500000000004ull;
constexpr uint64_t kSaltSound  = 0x534F554E44000006ull;
constexpr uint64_t kSaltPad    = 0x5041440000000007ull;
constexpr uint64_t kSaltMode   = 0x4D4F44450000008ull;   ///< the material of a borrowed mode (16.09.2026)

using Allowed = std::vector<std::vector<uint8_t>>;

/** @name The lead's ambitus: the window every lead note is drawn in, as intervals to its root. @{ */
constexpr int kLeadRelLo = -5;
constexpr int kLeadRelHi = 14;
/** @} */

int sym(int rel) { return PitchModel::symbol(std::clamp(rel, kCorpusRelMin, kCorpusRelMax)); }

/**
 * @brief The measured tension curve of a role: deviation of Lerdahl instability from the role's mean.
 *
 * Counted by Tools/corpus/measure_tension.py over 655 deduplicated corpus lines (655 of 666; the
 * near-duplicate pass of Tools/train/dataset.py). @c beat holds the mean instability of each beat of
 * the bar minus the role's own mean over the four beats, and @c parity half the paired per-line
 * difference between the odd and the even bar of a two-bar pair, so that the even bar gets
 * @c -parity and the odd bar @c +parity.
 *
 * | role | beat 1 | beat 2 | beat 3 | beat 4 | beat 4 - beat 1 (paired, 95 %) | bar parity (paired, 95 %) |
 * |------|--------|--------|--------|--------|--------------------------------|---------------------------|
 * | acid | 0.994  | 1.198  | 1.270  | 1.230  | +0.446 [+0.164, +0.760]        | +0.139 [+0.044, +0.248]   |
 * | lead | 1.169  | 1.318  | 1.406  | 1.636  | +0.552 [+0.318, +0.788]        | +0.199 [+0.074, +0.323]   |
 * | arp  | 0.960  | 1.000  | 1.048  | 1.219  | +0.287 [+0.212, +0.360]        | +0.116 [+0.089, +0.144]   |
 *
 * Every interval excludes zero, so both effects are real. What the corpus does **not** show is the
 * eight-bar schedule the review proposed: there is no peak in bar 7, and the last note of a bar is
 * *less* often a tonic or a fifth as a four-bar group goes on (arp 0.731 to 0.596, acid 0.638 to
 * 0.538, lead 0.582 to 0.484), the opposite of a phrase-final resolution. So only these two effects
 * are implemented.
 */
struct TensionCurve { double beat[4]; double parity; };
constexpr TensionCurve kTensionCurve[kNumCorpusRoles] = {
    { { -0.179, 0.025, 0.097, 0.057 }, 0.070 },   // acid
    { { -0.213, -0.064, 0.024, 0.254 }, 0.100 },  // lead
    { { -0.097, -0.057, -0.009, 0.162 }, 0.058 }, // arp
};

/**
 * @brief How hard the measured curve tilts a position's weights, per role.
 *
 * The weights are an exponential tilt @c exp(kTensionTilt * D * instability). A first-order estimate
 * says the expected instability at a position then moves by about @c tilt * D * Var(instability)
 * over the position's allowed set, which for the seven degrees of a minor mode is roughly 1.27 and
 * would put the tilt near 0.8. **Measured, that overshoots by three to one**: at 0.8 the composer's
 * own lines came out at a lead beat contrast of +1.13 against the corpus's +0.55 and a lead parity
 * of +0.74 against +0.20 -- the model's own distribution reacts far more sharply to a tilt than a
 * uniform allowed set would. The tilt was therefore calibrated backwards from the measurement, over
 * 400 tracks per setting:
 *
 * | tilt   | acid beat | lead beat | acid parity | lead parity |
 * |--------|-----------|-----------|-------------|-------------|
 * | 0.00   | +0.147    | +0.381    | +0.062      | +0.203      |
 * | 0.20   | +0.228    | +0.558    | +0.079      | +0.276      |
 * | 0.28   | +0.235    | +0.603    | +0.106      | +0.316      |
 * | 0.40   | +0.265    | +0.696    | +0.101      | +0.376      |
 * | corpus | +0.446    | +0.552    | +0.139      | +0.199      |
 *
 * 0.20 puts the lead's beat contrast on the corpus value and all four inside the corpus's own 95 %
 * intervals; 0.40 pushes the lead's parity out of its interval, and 0 -- the behaviour before this
 * round -- leaves the acid's beat contrast below its interval. The acid moves least, because its own
 * corpus model already dominates its within-bar shape. The self test re-measures all four.
 *
 * **The arp is 0 on purpose.** Its allowed set is the chord and nothing else (PLAN 6.5: every arp
 * note is a chord tone), and within one triad the three pitch classes carry Lerdahl instabilities of
 * only 0, 2 and 1. At a tilt of 0.8 the arp's measured beat contrast came out at +0.017 against a
 * corpus value of +0.287: there is simply not enough stability variance inside a triad for the curve
 * to act on. Carrying it would mean letting the arp off the chord, which is a bigger change than the
 * curve is worth, so the arp's curve is reported and not implemented.
 */
constexpr double kTensionTilt[kNumCorpusRoles] = { 0.20, 0.20, 0.0 };

/**
 * @brief How much of the tilt the bar-parity term carries, relative to the within-bar term.
 *
 * Measured, not assumed. With one gain for both halves of the curve the calibrated tilt put the beat
 * contrast where the corpus has it (+0.47 against +0.45 for the acid, +0.43 against +0.55 for the
 * lead) but the bar parity at +0.33 and +0.37 against corpus means of +0.14 and +0.20 -- outside the
 * corpus's own intervals. The reason is structural: the parity term is a constant offset over a
 * whole bar and moves a line's mean about twice as hard as a difference *within* a bar does. So the
 * parity term carries half the gain, which is what puts both contrasts inside both intervals.
 */
constexpr double kParityGain = 0.5;

/**
 * @brief The curve's tilt at one step of a pattern: the measured deviation times the role's tilt.
 * @param role  which role's measured curve
 * @param step  sixteenths from the start of the pattern
 * @param bars  the pattern's length in bars; under two the bar-parity term has nowhere to live and
 *              is dropped, because a one-bar cell is replayed in every bar and would otherwise be
 *              biased to the stable side of a contrast that is about the *pair* of bars.
 * @return 0 where the curve is not implemented (the arp), which leaves the allowed sets exactly as
 *         they were before 16.09.2026.
 */
double tensionAt(CorpusRoleId role, int step, int bars)
{
    const int r = static_cast<int>(role);
    if (kTensionTilt[r] == 0.0) return 0.0;
    const TensionCurve& c = kTensionCurve[r];
    double d = c.beat[((step % 16) + 16) % 16 / 4];
    if (bars >= 2) d += kParityGain * (((step / 16) % 2 == 0) ? -c.parity : c.parity);
    return kTensionTilt[r] * d;
}

/** @brief Index drawn from non-negative weights. */
int drawIndex(Rng& r, const double* w, int n)
{
    double total = 0.0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0.0) return r.below(n);
    double x = static_cast<double>(r.uniform()) * total;
    for (int i = 0; i < n; ++i) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}

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
void quantise(const std::vector<double>& w, std::vector<uint8_t>& out)
{
    double mx = 0.0;
    for (double v : w) mx = std::max(mx, v);
    if (mx <= 0.0) return;
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] > 0.0) out[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(200.0 * w[i] / mx)), 1, 255));
}

/**
 * @brief Allowed symbols: scale tones (relative to the key) in [lo, hi] semitones above the part root.
 * @param colour  0 = no colour weighting; > 0 = the colour tones lifted by 1 + 2 * colour (the
 *                dissonance side of the energy arc, Farbood 2012)
 * @param tension the measured tension curve's deviation at this position (tensionAt); 0 leaves the
 *                plain integer weights the sampler has used since Phase 5 exactly as they were
 */
std::vector<uint8_t> scaleSet(int scale, int rootOffset, int lo, int hi, float colour = 0.0f, double tension = 0.0)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    if (tension == 0.0) {
        const int base = colour > 0.0f ? 8 : 1;
        for (int rel = lo; rel <= hi; ++rel) {
            if (rel < kCorpusRelMin || rel > kCorpusRelMax || !inScale(scale, rel + rootOffset)) continue;
            int w = base;
            if (colour > 0.0f && isColourTone(scale, rel + rootOffset))
                w = std::clamp(static_cast<int>(std::lround(8.0f * (1.0f + 2.0f * colour))), 1, 255);
            a[static_cast<size_t>(sym(rel))] = static_cast<uint8_t>(w);
        }
        return a;
    }
    // The two weightings multiply: the arc's colour lifts the genre's colour tones wherever the arc
    // is high, the measured curve tilts the whole position towards or away from instability.
    std::vector<double> w(static_cast<size_t>(kCorpusAlphabet), 0.0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax || !inScale(scale, rel + rootOffset)) continue;
        double v = std::exp(tension * lerdahlInstability(rel + rootOffset));
        if (colour > 0.0f && isColourTone(scale, rel + rootOffset)) v *= 1.0 + 2.0 * static_cast<double>(colour);
        w[static_cast<size_t>(sym(rel))] = v;
    }
    quantise(w, a);
    return a;
}

/** @brief Allowed symbols: the chord's tones in [lo, hi], optionally tilted by the tension curve. */
std::vector<uint8_t> chordSet(int scale, int degree, int rootOffset, int lo, int hi, double tension = 0.0)
{
    int pcs[3];
    chordTones(scale, degree, pcs);
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    std::vector<double> w(static_cast<size_t>(kCorpusAlphabet), 0.0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax) continue;
        const int pc = ((rel + rootOffset) % 12 + 12) % 12;
        if (pc != pcs[0] && pc != pcs[1] && pc != pcs[2]) continue;
        if (tension == 0.0) a[static_cast<size_t>(sym(rel))] = 1;
        else w[static_cast<size_t>(sym(rel))] = std::exp(tension * lerdahlInstability(rel + rootOffset));
    }
    if (tension != 0.0) quantise(w, a);
    return a;
}

std::vector<uint8_t> single(int rel)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    a[static_cast<size_t>(sym(rel))] = 1;
    return a;
}

/** @brief Onsets over @p steps sixteenths from the role's corpus counts; the first step always sounds. */
std::vector<int> drawOnsets(Rng& r, const CorpusRole& role, int steps)
{
    std::vector<int> on;
    int prev = 1;
    for (int s = 0; s < steps; ++s) {
        const auto& c = role.onset[s % 16][prev];
        const double total = static_cast<double>(c[0]) + c[1];
        const double pOn = total > 0.0 ? c[1] / total : 0.5;
        const bool hit = s == 0 || r.uniform() < pOn;
        if (hit) on.push_back(s);
        prev = hit ? 1 : 0;
    }
    return on;
}

/** @brief A length in sixteenths from the role's counts at @p step, capped by @p gap. */
int drawLength(Rng& r, const CorpusRole& role, int step, int gap)
{
    double w[8];
    for (int k = 0; k < 8; ++k) w[k] = role.length[step % 16][k + 1];
    return std::clamp(drawIndex(r, w, 8) + 1, 1, std::max(1, gap));
}

/** @brief The uniform source the sampler draws from. */
struct Uniform {
    Rng* r;
    double operator()() const { return static_cast<double>(r->uniform()); }
};

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
};

/**
 * @brief The per-note conditioning of a line, from the onset steps the maker has already drawn.
 *
 * docs/MODEL_FORMAT.md section 3: the stage-B model is told, for the note it is about to predict,
 * where in the bar it sits, which bar of the pattern that is, how far the next note is and roughly
 * how far into the line it stands. All four are known before any pitch exists, because all three
 * makers here draw the rhythm first. The steps are relative to the pattern the line belongs to --
 * one acid pattern, one two-bar lead window, one arp cell -- not to the track.
 */
std::vector<NoteCond> noteConds(const std::vector<int>& steps)
{
    std::vector<NoteCond> c(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        const int s = steps[i];
        const bool hasNext = i + 1 < steps.size();
        c[i].step = (s % 16 + 16) % 16;
        c[i].bar = (s / 16 % 8 + 8) % 8;
        c[i].gap = noteGapCode(s, hasNext ? steps[i + 1] : 0, hasNext);
        c[i].idx = noteIndexBucket(static_cast<int>(i));
    }
    return c;
}

/**
 * @brief Draws pitches for a sequence of allowed sets; falls back to the lowest allowed symbol of each.
 * @param steps the onset step of each position inside its pattern (the conditioning of stage B)
 * @param bars  the pattern's length in bars
 */
std::vector<int> drawPitches(const PitchModel& model, CorpusRoleId role, const DrawSource& src,
                             const Allowed& allowed, const std::vector<int>& steps, int bars,
                             int ctx2, int ctx1, double temperature, Rng& r)
{
    std::vector<int> syms;
    bool ok = false;
    if (src.nn != nullptr && steps.size() == allowed.size()) {
        const std::vector<NoteCond> cond = noteConds(steps);
        NeuralStepper stepper{ src.nn, static_cast<int>(role), src.style, bars, &cond, 0 };
        // Stage B has no order-2 state: it is primed with the previous symbol only, and before the
        // first note that is the start token docs/MODEL_FORMAT.md section 3 fixes at the interval 0.
        ok = sampleMasked(stepper, allowed, sym(ctx2), sym(ctx1), temperature, Uniform{ &r }, syms);
    } else {
        ok = sampleConstrained(model, allowed, sym(ctx2), sym(ctx1), temperature, Uniform{ &r }, syms);
    }
    if (!ok) {
        syms.clear();
        for (const auto& a : allowed) {
            int s = 0;
            while (s < kCorpusAlphabet - 1 && !a[static_cast<size_t>(s)]) ++s;
            syms.push_back(s);
        }
    }
    std::vector<int> rels;
    for (int s : syms) rels.push_back(PitchModel::rel(s));
    return rels;
}

int median(const uint32_t* counts, int n)
{
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += counts[i];
    uint64_t acc = 0;
    for (int i = 0; i < n; ++i) { acc += counts[i]; if (acc * 2 >= total && total > 0) return i; }
    return 12;
}

// ---------------------------------------------------------------------------------------------

void makeChords(MelodyPlan& m, int scale, uint64_t seed, double temperature, const StyleProfile& style)
{
    Rng r;
    r.seed(seed ^ kSaltChords);
    m.chordBars = r.uniform() < 0.6f ? 2 : 4;
    m.chordDegree[0] = 0;
    for (int i = 1; i < 4; ++i) {
        const int prev = m.chordDegree[i - 1];
        if (r.uniform() < 0.35f) { m.chordDegree[i] = prev; continue; }
        double w[7] = {};
        for (int d = 0; d < 7; ++d) {
            if (d == prev) continue;
            const double p = chordTransition(scaleDegree(scale, prev), scaleDegree(scale, d));
            // The corpus successions are the base (0 to 5, 0 to 7 and 0 to 8 are its commonest moves);
            // the style profile adds the pendulum moves of the literature -- Goa's i to bII and i to
            // bVII -- on top of them rather than replacing them. The extras are scaled to a sixth of a
            // probability, which is the size of a common corpus move.
            const int move = ((scaleDegree(scale, d) - scaleDegree(scale, prev)) % 12 + 12) % 12;
            w[d] = (p > 0.0 ? std::pow(p, 1.0 / temperature) : 0.0) + 0.15 * style.chordExtra[move];
        }
        m.chordDegree[i] = drawIndex(r, w, 7);
    }
}

void makeAcid(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, const DrawSource& src)
{
    Rng r;
    r.seed(seed ^ kSaltAcid);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Acid)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Acid);
    m.acidSteps = r.uniform() < 0.7f ? 16 : 32;
    const int steps = m.acidSteps;
    // Acid lines are busy: at least 40 % of the steps sound.
    std::vector<int> on;
    for (int tries = 0; tries < 12; ++tries) {
        on = drawOnsets(r, role, steps);
        if (static_cast<int>(on.size()) * 10 >= steps * 4) break;
    }
    if (static_cast<int>(on.size()) * 10 < steps * 4) { on.clear(); for (int s = 0; s < steps; s += 2) on.push_back(s); }
    const int ambitus = std::clamp(median(role.ambitus, 37), 7, 19);
    const int rootOffset = ((m.root[0] - key) % 12 + 12) % 12;
    const int bars = steps / 16;
    Allowed allowed;
    for (size_t i = 0; i < on.size(); ++i)
        allowed.push_back(i == 0 ? single(0)
                                 : scaleSet(scale, rootOffset, 0, ambitus, 0.0f, tensionAt(CorpusRoleId::Acid, on[i], bars)));
    const std::vector<int> rels = drawPitches(model, CorpusRoleId::Acid, src, allowed, on, bars, 0, 0, temperature, r);

    std::vector<MelodyNote>& a = m.acid[0];
    a.clear();
    /**
     * Accents cluster; they are not drawn independently. The TB-303's accent charges a capacitor with
     * a time constant of 150 ms (Acid.cpp, kSweepTau), so a lone accent charges the sweep and lets it
     * discharge again while accents that follow one another charge it before it has fallen and their
     * sweeps climb -- the "wow" of an accented run. An independent draw produces that run only by
     * chance.
     *
     * The size of the clustering is measured, not assumed (Tools/ref_accent_runs.py, 16.09.2026, on
     * the bought MIDI packs, per track and restricted to loops of at most eight bars so that the
     * section dynamics of whole arrangements cannot pose as accents). For the acid role, with the
     * accent definition the corpus tables already use -- a velocity at or above the loop's median
     * plus 10 -- the probability of an accent on the next onset is 0.493 after an accent (34 of 69)
     * against 0.197 after a plain note (35 of 178): a lift of 2.51, with 95 % Wilson intervals of
     * [0.378, 0.608] and [0.145, 0.261] that do not overlap. The scale-free two-means definition of
     * the same tool gives 0.686 against 0.241, a lift of 2.84, on the same nine loops. kAccentLift is
     * the smaller and more conservative of the two.
     *
     * The base is thin and is stated as thin: 62 of the 74 acid loops in the packs carry no velocity
     * variation at all (spread 0 to 4), which is why the acid row of CorpusTables.cpp counts zero
     * accents, and the lift rests on the nine that do. It does *not* support the 0.75 an external
     * review proposed for the follow-up probability; the measured follow-up probability is 0.49.
     *
     * The chain keeps the marginal accent rate of each step position where it was, so this changes how
     * the accents are *distributed* and not how many there are: for a target rate m at a position and
     * the lift L, the probability after a plain note is m / (1 - m + L m) and after an accent L times
     * that. With the rates below (0.12, 0.2, 0.32) that is 0.102/0.256, 0.154/0.386 and 0.216/0.542.
     *
     * **What it does to the sweep, measured rather than assumed, and it is less than the argument
     * promises.** Playing the composed patterns into a real acid voice on their own step grid (self
     * test, section "acid colour"), the mean charge of the sweep capacitor under an accented note goes
     * from 0.248 to 0.261 and its peak from 0.406 to 0.405 -- 5 % more charge, that is 0.011 of an
     * octave of cutoff at the default Accent and Resonance. The mechanism of the argument is right and
     * its size is not: at 145 BPM a sixteenth lasts 103 ms against the capacitor's 150 ms, so accents
     * two steps apart already find it far from discharged, and nearly doubling the number of
     * *adjacent* accents (0.20 to 0.39) adds little to what was already there. The change stays
     * because the corpus says the accents cluster, not because it makes the sweep climb.
     */
    constexpr float kAccentLift = 2.51f;
    bool prevAccent = false;
    for (size_t i = 0; i < on.size(); ++i) {
        const int step = on[i];
        const int next = i + 1 < on.size() ? on[i + 1] : steps;
        const int gap = next - step;
        MelodyNote n;
        n.step = static_cast<int16_t>(step);
        n.rel = static_cast<int8_t>(rels[i]);
        n.len = static_cast<int16_t>(drawLength(r, role, step, gap));
        const int pos = step % 4;
        const float pAccent = pos == 2 ? 0.32f : (pos == 0 ? 0.12f : 0.2f);
        const float pPlain = pAccent / (1.0f - pAccent + kAccentLift * pAccent);
        prevAccent = r.uniform() < (prevAccent ? std::min(1.0f, kAccentLift * pPlain) : pPlain);
        if (prevAccent) n.flags |= kNoteAccent;
        if (gap <= 2 && i + 1 < on.size() && r.uniform() < 0.2f) { n.flags |= kNoteSlide; n.len = static_cast<int16_t>(gap); }
        n.velocity = (n.flags & kNoteAccent) ? 120 : 88;
        a.push_back(n);
    }
    // B: two or three notes redrawn given their neighbours, one accent moved.
    std::vector<MelodyNote>& b = m.acid[1];
    b = a;
    if (a.size() >= 4) {
        const int changes = 2 + r.below(2);
        std::vector<uint8_t> redraw(a.size(), 0);
        for (int c = 0; c < changes; ++c) redraw[static_cast<size_t>(1 + r.below(static_cast<int>(a.size()) - 1))] = 1;
        Allowed vary;
        for (size_t i = 0; i < a.size(); ++i)
            vary.push_back(redraw[i] ? scaleSet(scale, rootOffset, 0, ambitus, 0.0f, tensionAt(CorpusRoleId::Acid, on[i], bars))
                                     : single(a[i].rel));
        const std::vector<int> vr = drawPitches(model, CorpusRoleId::Acid, src, vary, on, bars, 0, 0, temperature, r);
        for (size_t i = 0; i < b.size(); ++i) b[i].rel = static_cast<int8_t>(vr[i]);
        MelodyNote& moved = b[static_cast<size_t>(r.below(static_cast<int>(b.size())))];
        moved.flags ^= kNoteAccent;
        moved.velocity = (moved.flags & kNoteAccent) ? 120 : 88;
    }
}

/** @brief The chord degree at a phrase step of lead window @p w. */
int chordAtStep(const MelodyPlan& m, int window, int step)
{
    return m.chordDegree[chordIndexAt(m, window * 8 + step / 16)];
}

void makeLead(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, const DrawSource& src)
{
    Rng r;
    r.seed(seed ^ kSaltLead);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Lead)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Lead);
    const int rootOffset = ((m.root[1] - key) % 12 + 12) % 12;
    constexpr int lo = kLeadRelLo, hi = kLeadRelHi;
    m.colour = colour;

    auto rhythm = [&]() {
        std::vector<int> on;
        for (int tries = 0; tries < 12; ++tries) {
            on = drawOnsets(r, role, 32);
            if (on.size() >= 4 && on.size() <= 14) break;
        }
        if (on.size() < 4) on = { 0, 6, 12, 16, 22, 28 };
        if (on.size() > 14) on.resize(14);
        return on;
    };
    // Constraints of one note: chord tones on strong steps, scale tones elsewhere. Both carry the
    // measured tension curve of the lead; the motif is two bars long, so the bar-parity term applies.
    auto constraintAt = [&](int window, int step) {
        const double t = tensionAt(CorpusRoleId::Lead, step, 2);
        return step % 8 == 0 ? chordSet(scale, chordAtStep(m, window, step), rootOffset, lo, hi, t)
                             : scaleSet(scale, rootOffset, lo, hi, colour, t);
    };
    auto fits = [&](int window, int step, int rel) {
        return constraintAt(window, step)[static_cast<size_t>(sym(rel))] != 0;
    };

    const std::vector<int> motifRhythm = rhythm();
    std::vector<int> motif;   // A's pitches, drawn once for window 0
    for (int w = 0; w < 2; ++w) {
        std::vector<MelodyNote>& ph = m.lead[w];
        ph.clear();
        std::vector<int> steps, rels;
        // A (bars 0-1).
        Allowed al;
        for (int s : motifRhythm) al.push_back(constraintAt(w, s));
        if (motif.empty()) motif = drawPitches(model, CorpusRoleId::Lead, src, al, motifRhythm, 2, 0, 0, temperature, r);
        {
            // A keeps the motif where it fits this window's chords and redraws the rest.
            Allowed keep;
            for (size_t i = 0; i < motifRhythm.size(); ++i)
                keep.push_back(fits(w, motifRhythm[i], motif[i]) ? single(motif[i]) : al[i]);
            const std::vector<int> a = drawPitches(model, CorpusRoleId::Lead, src, keep, motifRhythm, 2, 0, 0, temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(motifRhythm[i]); rels.push_back(a[i]); }
        }
        // A' (bars 2-3): same rhythm, notes kept where they fit.
        {
            Allowed keep;
            for (size_t i = 0; i < motifRhythm.size(); ++i) {
                const int s = motifRhythm[i] + 32;
                keep.push_back(fits(w, s, motif[i]) ? single(motif[i]) : constraintAt(w, s));
            }
            const int c1 = rels.size() >= 1 ? rels.back() : 0, c2 = rels.size() >= 2 ? rels[rels.size() - 2] : c1;
            const std::vector<int> a = drawPitches(model, CorpusRoleId::Lead, src, keep, motifRhythm, 2, c2, c1, temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(motifRhythm[i] + 32); rels.push_back(a[i]); }
        }
        // B (bars 4-5): its own rhythm, continuing from A'.
        {
            const std::vector<int> br = rhythm();
            Allowed al2;
            for (int s : br) al2.push_back(constraintAt(w, s + 64));
            const std::vector<int> a = drawPitches(model, CorpusRoleId::Lead, src, al2, br, 2, rels[rels.size() - 2], rels.back(), temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(br[i] + 64); rels.push_back(a[i]); }
        }
        // A'' (bars 6-7): the motif, its last notes redrawn, ending on a chord tone -- and since
        // 16.09.2026 one systematic transformation on top of that (Melody.h, MotifOperator). The
        // operator is drawn per phrase from the phrase's own point in the lead's random stream, so it
        // is as deterministic as everything else here.
        {
            static const double kOperatorWeights[kNumMotifOperators] = { 0.40, 0.20, 0.20, 0.20 };
            const int op = drawIndex(r, kOperatorWeights, kNumMotifOperators);
            m.leadOperator[w] = op;
            // The phase shift is a rotation of the two-bar cell by one sixteenth: the motif loops
            // every two bars, so moving it on by one step and letting the last note wrap round is
            // what "displaced by a sixteenth" means for a loop. Every note that sat on a beat then
            // sits just after one, which is the syncopation the operator exists for.
            std::vector<int> rh = motifRhythm;
            if (op == static_cast<int>(MotifOperator::PhaseShift)) {
                for (int& s : rh) s = (s + 1) % 32;
                std::sort(rh.begin(), rh.end());
            }
            const size_t n = rh.size();
            const size_t free = std::max<size_t>(2, (n * 2) / 5);
            Allowed keep, cons;
            for (size_t i = 0; i < n; ++i) {
                const int s = rh[i] + 96;
                const double t = tensionAt(CorpusRoleId::Lead, s, 2);
                cons.push_back(i + 1 == n ? chordSet(scale, chordAtStep(m, w, s), rootOffset, lo, hi, t)
                                          : constraintAt(w, s));
                if (i + free >= n || i >= motif.size() || !fits(w, s, motif[i])) keep.push_back(cons.back());
                else keep.push_back(single(motif[i]));
            }
            std::vector<int> a = drawPitches(model, CorpusRoleId::Lead, src, keep, rh, 2, rels[rels.size() - 2], rels.back(), temperature, r);
            if (op == static_cast<int>(MotifOperator::Expand)) {
                std::vector<int> ex;
                // The expansion runs against the *constraint* sets, not against `keep`: a position
                // that keeps the motif's note is a single symbol and could not be widened at all.
                if (expandContour(a, cons, lo, hi, ex)) a = ex;
                else m.leadOperator[w] = static_cast<int>(MotifOperator::None);
            }
            if (op == static_cast<int>(MotifOperator::OctaveJump)) {
                // Single offbeat sixteenths thrown an octave up: the Goa lead idiom. An octave keeps
                // the pitch class, so a jumped note is still in the mode and still a chord tone where
                // it was one; the ceiling keeps the masking rule against the arp workable.
                int jumps = 0;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (rh[i] % 2 == 0 || a[i] + 12 > kCorpusRelMax || m.root[1] + a[i] + 12 > 96) continue;
                    if (r.uniform() < 0.5f) { a[i] += 12; ++jumps; }
                }
                m.leadJumps[w] = jumps;
                if (jumps == 0) m.leadOperator[w] = static_cast<int>(MotifOperator::None);
            }
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(rh[i] + 96); rels.push_back(a[i]); }
        }
        for (size_t i = 0; i < steps.size(); ++i) {
            const int next = i + 1 < steps.size() ? steps[i + 1] : 128;
            MelodyNote n;
            n.step = static_cast<int16_t>(steps[i]);
            n.len = static_cast<int16_t>(drawLength(r, role, steps[i], next - steps[i]));
            n.rel = static_cast<int8_t>(rels[i]);
            n.velocity = steps[i] % 8 == 0 ? 104 : 92;
            ph.push_back(n);
        }
    }
}

void makeArp(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, const DrawSource& src)
{
    Rng r;
    r.seed(seed ^ kSaltArp);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Arp)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Arp);
    const int rootOffset = ((m.root[2] - key) % 12 + 12) % 12;
    // 16.09.2026: two more step families. Euclidean selection (Toussaint, "The Euclidean algorithm
    // generates traditional musical rhythms", BRIDGES 2005) reuses the percussion's own generator,
    // and the polymeter is a three-sixteenth cell that precesses against the 4/4 bar.
    static const double kArpStyleWeights[kNumArpStyles] = { 0.30, 0.12, 0.10, 0.12, 0.24, 0.12 };
    m.arpStyle = drawIndex(r, kArpStyleWeights, kNumArpStyles);
    m.arpPolymeter = m.arpStyle == static_cast<int>(ArpStyle::Polymeter);
    m.arpOctaveJump = r.uniform() < 0.3f;
    const int span = r.uniform() < 0.6f ? 12 : 19;
    std::vector<int> on;
    if (m.arpStyle == static_cast<int>(ArpStyle::Euclid)) {
        // E(5,16) and E(7,16) are the two the plan names; their neighbours E(4,16), E(6,16) and
        // E(8,16) are the rest of the family and carry a third of the weight between them.
        static const double kPulseWeights[5] = { 0.10, 0.30, 0.15, 0.30, 0.15 };
        m.arpPulses = 4 + drawIndex(r, kPulseWeights, 5);
        // The rotation at a moderate syncopation, exactly as a Euclidean percussion lane chooses
        // its own (Rhythm.cpp): Sioros, Miron, Davies, Gouyon and Madison ("Syncopation creates the
        // sensation of groove in synthesized music examples", Frontiers in Psychology 2014) found
        // groove rising with moderate syncopation, so neither extreme is taken. Ties go to the
        // lowest rotation, so the choice is a function of the pulses alone and cannot drift.
        auto syncOf = [&](int rot) {
            const std::vector<bool> e = euclid(m.arpPulses, kStepsPerBar, rot);
            bool st[kStepsPerBar];
            for (int i = 0; i < kStepsPerBar; ++i) st[i] = e[static_cast<size_t>(i)];
            return lhlSyncopation(st);
        };
        int lo = 1 << 30, hi = -(1 << 30);
        for (int rot = 0; rot < kStepsPerBar; ++rot) { const int s = syncOf(rot); lo = std::min(lo, s); hi = std::max(hi, s); }
        const int target = (lo + hi) / 2;
        int best = 0, bestD = 1 << 30;
        for (int rot = 0; rot < kStepsPerBar; ++rot) {
            const int d = std::abs(syncOf(rot) - target);
            if (d < bestD) { bestD = d; best = rot; }
        }
        m.arpRotation = best;
        const std::vector<bool> e = euclid(m.arpPulses, kStepsPerBar, m.arpRotation);
        for (int s = 0; s < kStepsPerBar; ++s) if (e[static_cast<size_t>(s)]) on.push_back(s);
        if (on.empty()) on.push_back(0);
    } else if (m.arpPolymeter) {
        // The cell is three sixteenths long; composeMelodyBar reads it at the absolute sixteenth of
        // the track, so it starts one step later in every bar and comes home every three bars.
        on = { 0, 1, 2 };
    } else {
        for (int tries = 0; tries < 12; ++tries) {
            on = drawOnsets(r, role, 16);
            if (on.size() >= 10) break;
        }
        if (on.size() < 10) { on.clear(); for (int s = 0; s < 16; ++s) on.push_back(s); }
    }
    const bool drawn = m.arpStyle == static_cast<int>(ArpStyle::Corpus)
                    || m.arpStyle == static_cast<int>(ArpStyle::Euclid) || m.arpPolymeter;
    for (int c = 0; c < 4; ++c) {
        const std::vector<uint8_t> set = chordSet(scale, m.chordDegree[c], rootOffset, 0, span);
        std::vector<int> tones;
        for (int s = 0; s < kCorpusAlphabet; ++s) if (set[static_cast<size_t>(s)]) tones.push_back(PitchModel::rel(s));
        if (tones.empty()) tones.push_back(0);
        std::vector<int> rels;
        if (drawn) {
            // The arp's cell is one bar, so only the beat half of the measured tension curve applies.
            Allowed al;
            for (int s : on) al.push_back(chordSet(scale, m.chordDegree[c], rootOffset, 0, span,
                                                   tensionAt(CorpusRoleId::Arp, s, 1)));
            rels = drawPitches(model, CorpusRoleId::Arp, src, al, on, 1, tones[0], tones[0], temperature, r);
        } else {
            const int t = static_cast<int>(tones.size());
            for (size_t i = 0; i < on.size(); ++i) {
                const int k = static_cast<int>(i);
                int idx = 0;
                if (m.arpStyle == 1) idx = k % t;
                else if (m.arpStyle == 2) idx = t - 1 - k % t;
                else { const int period = std::max(1, 2 * t - 2); const int ph = k % period; idx = ph < t ? ph : period - ph; }
                rels.push_back(tones[static_cast<size_t>(idx)]);
            }
        }
        std::vector<MelodyNote>& cell = m.arp[c];
        cell.clear();
        for (size_t i = 0; i < on.size(); ++i) {
            MelodyNote n;
            n.step = static_cast<int16_t>(on[i]);
            n.len = 1;
            n.rel = static_cast<int8_t>(rels[i]);
            n.velocity = on[i] % 4 == 0 ? 104 : 84;
            cell.push_back(n);
        }
    }
}

/**
 * @brief Widens [lo, hi] to hold every note of @p notes over @p root.
 * @param relCeiling notes whose interval to the root is above this are left out; that is how the
 *                   lead's octave jumps stay out of the range the masking rule works with.
 */
void pitchRange(const std::vector<MelodyNote>& notes, int root, int& lo, int& hi, int relCeiling = 127)
{
    for (const MelodyNote& n : notes) {
        if (n.rel > relCeiling) continue;
        lo = std::min(lo, root + n.rel);
        hi = std::max(hi, root + n.rel);
    }
}

/** @brief The pad's gate pattern and the pitch ranges the masking rule works on. */
void makeRangesAndPad(MelodyPlan& m, uint64_t seed)
{
    // Octave-jumped notes (MotifOperator::OctaveJump) are left out of the lead's range on purpose.
    // The range exists for one thing: the masking rule that keeps the arp out of the lead's register
    // (Form.cpp). A single displaced sixteenth is an excursion, not a register, and letting it drive
    // the rule pushed the arp a whole octave higher and past the ceiling -- measured, the arp was
    // silenced in 53 % of drops with the jumps counted and in 26 % without. Every drawn lead note
    // sits at or under kLeadRelHi, so anything above it is a jump and nothing else.
    for (const auto& ph : m.lead) pitchRange(ph, m.root[1], m.leadLo, m.leadHi, kLeadRelHi);
    for (const auto& cell : m.arp) pitchRange(cell, m.root[2], m.arpLo, m.arpHi);
    // Every borrowed mode counts towards the ranges as well. The masking rule between lead and arp
    // is decided once per section from these numbers (Form.cpp), and a section may be playing a
    // borrowed mode; taking the union keeps the rule conservative rather than letting a recoloured
    // note slip past it.
    for (const ModeMaterial& mm : m.mode) {
        if (!mm.built) continue;
        for (const auto& ph : mm.lead) pitchRange(ph, m.root[1], m.leadLo, m.leadHi, kLeadRelHi);
        for (const auto& cell : mm.arp) pitchRange(cell, m.root[2], m.arpLo, m.arpHi);
    }
    if (m.arpOctaveJump) m.arpHi += 12;
    if (m.leadLo > m.leadHi) { m.leadLo = m.root[1]; m.leadHi = m.root[1]; }
    if (m.arpLo > m.arpHi) { m.arpLo = m.root[2]; m.arpHi = m.root[2]; }
    Rng pr;
    pr.seed(seed ^ kSaltPad);
    static const double kPatternWeights[6] = { 0.35, 0.2, 0.15, 0.1, 0.15, 0.05 };
    m.padGatePattern = drawIndex(pr, kPatternWeights, 6);
}

} // namespace

const PitchModel& corpusPitchModel(CorpusRoleId role)
{
    static const PitchModel acidModel(CorpusRoleId::Acid), leadModel(CorpusRoleId::Lead), arpModel(CorpusRoleId::Arp);
    return role == CorpusRoleId::Acid ? acidModel : (role == CorpusRoleId::Lead ? leadModel : arpModel);
}

void chordTones(int scale, int degree, int out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = scaleDegree(scale, degree + 2 * i) % 12;
}

/**
 * @brief Widens every interval of a line while keeping its contour sign for sign.
 *
 * The contour-preserving expansion of PLAN 6.5: Schoenberg's developing variation as Frisch reads it
 * ("Brahms and the Principle of Developing Variation", University of California Press 1984) varies a
 * motif by keeping what makes it recognisable -- here the sequence of up, down and same -- and
 * changing the size of its steps. Each note is put at the first symbol its position allows that lies
 * *strictly further* from the note before than the parent's interval did, in the parent's direction;
 * that widens the step and can never turn the contour round. Where there is no room to widen, the
 * next allowed symbol in the same direction is taken, which keeps the sign but not the widening.
 * Where even that does not exist the whole expansion is refused, and the caller keeps the parent.
 */
bool expandContour(const std::vector<int>& parent, const std::vector<std::vector<uint8_t>>& allowed,
                   int lo, int hi, std::vector<int>& out)
{
    if (parent.size() < 2 || parent.size() != allowed.size()) return false;
    auto ok = [&](size_t i, int rel) {
        return rel >= lo && rel <= hi && rel >= kCorpusRelMin && rel <= kCorpusRelMax
            && allowed[i][static_cast<size_t>(sym(rel))] != 0;
    };
    out.assign(1, parent[0]);
    bool widened = false;
    for (size_t i = 1; i < parent.size(); ++i) {
        const int d = parent[i] - parent[i - 1];
        if (d == 0) {
            if (!ok(i, out.back())) { out.clear(); return false; }
            out.push_back(out.back());
            continue;
        }
        const int dir = d > 0 ? 1 : -1;
        int got = 0;
        bool found = false;
        for (int c = out.back() + d + dir; c >= lo && c <= hi; c += dir)
            if (ok(i, c)) { got = c; found = true; widened = true; break; }
        // No room to widen: the parent's own interval, if the position allows it. Anything nearer
        // would *narrow* the step, and an expansion that narrows is not an expansion, so the whole
        // variant is refused instead and the caller keeps the parent.
        if (!found && ok(i, out.back() + d)) { got = out.back() + d; found = true; }
        if (!found) { out.clear(); return false; }
        out.push_back(got);
    }
    return widened && out.size() == parent.size();
}

std::vector<int> melodyContour(const std::vector<MelodyNote>& notes)
{
    std::vector<int> out;
    for (size_t i = 1; i < notes.size(); ++i) {
        const int d = notes[i].rel - notes[i - 1].rel;
        out.push_back(d > 0 ? 1 : (d < 0 ? -1 : 0));
    }
    return out;
}

MelodyPlan makeMelodyPlan(const ParamStore& p, const StyleProfile& style, uint64_t seed, int key, int scale,
                          bool firstTrack, float colour, uint32_t scaleMask)
{
    const int cb = p.base(Module::Compose);
    const double temperature = p.get(cb + compose::MelodyTemperature);
    const float mv = p.get(cb + compose::MelodyVariation);
    MelodyPlan m;
    Rng r;
    r.seed(seed);
    // The style profile scales the knobs' amounts; Full-On, the default, scales everything by one.
    const float amounts[4] = { std::clamp(p.get(cb + compose::AcidAmount) * style.partAmount[0], 0.0f, 1.0f),
                               std::clamp(p.get(cb + compose::LeadAmount) * style.partAmount[1], 0.0f, 1.0f),
                               std::clamp(p.get(cb + compose::ArpAmount) * style.partAmount[2], 0.0f, 1.0f),
                               std::clamp(p.get(cb + compose::PadAmount) * style.partAmount[3], 0.0f, 1.0f) };
    for (int k = 0; k < kMelodyParts; ++k) m.present[k] = r.uniform() < amounts[k];
    // Every track has at least one melodic part unless all three amounts are zero: the likeliest one.
    if (!m.present[0] && !m.present[1] && !m.present[2]) {
        int best = 0;
        for (int k = 1; k < 3; ++k) if (amounts[k] * r.uniform() > amounts[best] * r.uniform()) best = k;
        m.present[best] = amounts[best] > 0.0f;
    }
    m.root[0] = kAcidLowest + ((key - 2) % 12 + 12) % 12;   // D3 .. C#4
    m.root[1] = 64 + ((key - 4) % 12 + 12) % 12;            // E4 .. D#5
    m.root[2] = 57 + ((key - 9) % 12 + 12) % 12;            // A3 .. G#4
    makeChords(m, scale, seed, temperature, style);
    // The predictive model of the melodic lines (Phase 8). Loaded once, on whichever thread composes
    // first -- never the audio thread -- and null whenever the knob says Markov or no weight file is
    // there, in which case every draw below is exactly the Phase 5 draw.
    DrawSource src;
    if (p.getInt(cb + compose::MelodyModel) == 1) {
        std::string note;
        src.nn = sharedMelodyModel(&note);
        if (src.nn == nullptr) {
            static bool told = false;
            if (!told) { told = true; std::printf("%s\n", note.c_str()); }
        }
    }
    // The style slot stays 0, "unknown": the MIDI packs carry no label that maps onto the five
    // style profiles, so every training line was labelled 0 and the inference side must match
    // (docs/MODEL_FORMAT.md section 3, the warning). The slot exists for a labelled corpus later.
    src.style = 0;
    m.scale = std::clamp(scale, 0, kNumScales - 1);
    makeAcid(m, key, scale, seed, temperature, src);
    makeLead(m, key, scale, seed, temperature, colour, src);
    makeArp(m, key, scale, seed, temperature, src);
    for (int c = 0; c < 4; ++c) m.padVoicing[c] = voiceChord(scale, m.chordDegree[c], key, c > 0 ? &m.padVoicing[c - 1] : nullptr);
    // Modal interchange (Form.h): the material of every mode a section of the form borrows. The
    // makers are run again with the *same* seed and another mode, so the rhythm, the accents and the
    // slides come out identical and only the pitches are recoloured -- interchange rather than a
    // second riff. The scratch plan carries the chords, the roots and the part switches, which are
    // the track's and do not belong to a mode. Nothing here can move the base mode's material: the
    // makers above have already run, and each of these calls starts its own Rng.
    for (int sc = 0; sc < kNumScales; ++sc) {
        if (sc == m.scale || (scaleMask & (1u << sc)) == 0) continue;
        MelodyPlan tmp = m;
        makeAcid(tmp, key, sc, seed, temperature, src);
        makeLead(tmp, key, sc, seed, temperature, colour, src);
        makeArp(tmp, key, sc, seed, temperature, src);
        for (int c = 0; c < 4; ++c)
            tmp.padVoicing[c] = voiceChord(sc, tmp.chordDegree[c], key, c > 0 ? &tmp.padVoicing[c - 1] : nullptr);
        ModeMaterial& mm = m.mode[sc];
        for (int i = 0; i < 2; ++i) { mm.acid[i] = tmp.acid[i]; mm.lead[i] = tmp.lead[i]; }
        for (int c = 0; c < 4; ++c) { mm.arp[c] = tmp.arp[c]; mm.padVoicing[c] = tmp.padVoicing[c]; }
        mm.built = true;
    }
    makeRangesAndPad(m, seed);

    Rng s;
    s.seed(seed ^ kSaltSound);
    if (!firstTrack) {
        for (float& x : m.recipe) x = (2.0f * s.uniform() - 1.0f) * mv;
        const float chance = std::clamp(p.get(cb + compose::SquelchChance) * style.squelchChance, 0.0f, 1.0f);
        m.acidSquelch = s.uniform() < chance ? 1 : 0;
        const float o = s.uniform();
        m.leadOsc = o < 0.65f ? static_cast<int>(PolyOsc::Supersaw) : (o < 0.85f ? static_cast<int>(PolyOsc::Fm) : static_cast<int>(PolyOsc::Va));
        static const int kLeft[3] = { 2, 1, 4 }, kRight[3] = { 3, 2, 1 };
        for (int k = 0; k < 3; ++k) { m.delay[k][0] = kLeft[s.below(3)]; m.delay[k][1] = kRight[s.below(3)]; }
    }
    return m;
}

BarPlan allPartsBar(const MelodyPlan& m)
{
    BarPlan bp;
    bp.parts = static_cast<uint8_t>((m.present[0] ? 1 : 0) | (m.present[1] ? 2 : 0) | (m.present[2] ? 4 : 0) | (m.present[3] ? 8 : 0));
    bp.partsNext = bp.parts;
    bp.type = SectionType::Drop;
    bp.energy = 1.0f;
    return bp;
}

/**
 * @brief The material a bar plays: the section's borrowed mode where it has one, else the track's.
 *
 * A section's mode is a property of the melodic layer alone (Form.h). A bar plan that names no mode
 * (-1: the transition's pad bar, the level-match probe of a single part) or names a mode the track
 * never built falls back to the track's own material, which is what every caller before 16.09.2026
 * got.
 */
static const ModeMaterial* materialOf(const MelodyPlan& m, int scale)
{
    if (scale < 0 || scale >= kNumScales || scale == m.scale) return nullptr;
    return m.mode[scale].built ? &m.mode[scale] : nullptr;
}

void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale,
                      const BarPlan& bp, std::vector<NoteEvent>& out)
{
    (void)scale;
    const ModeMaterial* mat = materialOf(m, bp.scale);
    const std::vector<MelodyNote>* acid = mat != nullptr ? mat->acid : m.acid;
    const std::vector<MelodyNote>* lead = mat != nullptr ? mat->lead : m.lead;
    const std::vector<MelodyNote>* arp = mat != nullptr ? mat->arp : m.arp;
    const std::vector<int>* voicing = mat != nullptr ? mat->padVoicing : m.padVoicing;
    const int cb = p.base(Module::Compose);
    const float swing = p.get(cb + compose::Swing);
    const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
    const uint8_t parts = bp.parts;
    // The cut of Grosz et al.: for its first beats a breakdown holds nothing but the reverb tail.
    const double cut = static_cast<double>(bp.cutBeats);
    auto beatOf = [&](int stepInBar) {
        double b = barBeat + stepInBar * 0.25;
        if (stepInBar % 2 == 1) b += swing * 0.25;
        return b;
    };
    auto emit = [&](Part part, const MelodyNote& n, int stepInBar, int pitch, double lengthBeats) {
        const double beat = beatOf(stepInBar);
        if (beat < barBeat + cut) return;
        NoteEvent e;
        e.beat = beat;
        e.length = static_cast<float>(lengthBeats);
        e.part = part;
        e.pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
        e.velocity = n.velocity;
        e.flags = n.flags;
        out.push_back(e);
    };

    if (parts & 1) {
        const int steps = m.acidSteps;
        const int phraseBars = steps == 16 ? 4 : 8;
        const int inPhrase = barInTrack % phraseBars;
        const bool variation = steps == 16 ? inPhrase == 3 : inPhrase >= 6;
        const int offset = steps == 16 ? 0 : (inPhrase % 2) * 16;
        for (const MelodyNote& n : acid[variation ? 1 : 0]) {
            if (n.step < offset || n.step >= offset + 16) continue;
            // A slide overlaps the next note by a little; other steps are short, as a 303's gate.
            MelodyNote note = n;
            // A slide that reaches into the next bar needs a note there to slide into; where the acid
            // stops at the end of a section it is a plain short note instead.
            if ((note.flags & kNoteSlide) != 0 && note.step - offset + note.len >= 16 && (bp.partsNext & 1) == 0)
                note.flags = static_cast<uint8_t>(note.flags & ~kNoteSlide);
            const double len = (note.flags & kNoteSlide) ? note.len * 0.25 + 0.03 : std::min<int>(note.len, 2) * 0.25 * 0.55;
            emit(Part::Acid, note, note.step - offset, m.root[0] + note.rel, len);
        }
    }
    if (parts & 2) {
        const int window = (barInTrack / 8) % 2;
        const int stepBase = (barInTrack % 8) * 16;
        for (const MelodyNote& n : lead[window]) {
            if (n.step < stepBase || n.step >= stepBase + 16) continue;
            emit(Part::Lead, n, n.step - stepBase, m.root[1] + n.rel + 12 * bp.leadOctave, n.len * 0.25 * 0.92);
        }
    }
    if (parts & 4) {
        const int c = chordIndexAt(m, barInTrack);
        const int jump = (m.arpOctaveJump && (barInTrack / 2) % 2 == 1) ? 12 : 0;
        const std::vector<MelodyNote>& cell = arp[c];
        if (m.arpPolymeter && cell.size() == 3) {
            // A three-sixteenth cell against a sixteen-sixteenth bar: the cell is read at the
            // *absolute* sixteenth of the track, so it starts one step later in every bar (16 mod 3
            // = 1) and comes home every three bars. Its accent -- the cell's first note -- precesses
            // with it, which is the whole audible point of a polymeter.
            for (int s = 0; s < kStepsPerBar; ++s) {
                const MelodyNote& n = cell[static_cast<size_t>((barInTrack + s) % 3)];
                emit(Part::Arp, n, s, m.root[2] + n.rel + jump + 12 * bp.arpOctave, 0.25 * 0.5);
            }
        } else {
            for (const MelodyNote& n : cell)
                emit(Part::Arp, n, n.step, m.root[2] + n.rel + jump + 12 * bp.arpOctave, 0.25 * 0.5);
        }
    }
    if ((parts & 8) && barInTrack % m.chordBars == 0) {
        MelodyNote held;
        held.velocity = 90;
        // Held to the next chord, a 64th short of it so a repeated pitch takes a fresh voice cleanly.
        // After a cut the chord starts where the cut ends, shortened by as much.
        const double length = m.chordBars * static_cast<double>(kBeatsPerBar) - 1.0 / 16.0 - cut;
        const int step = static_cast<int>(std::lround(cut * 4.0));
        for (int pitch : voicing[chordIndexAt(m, barInTrack)])
            emit(Part::Pad, held, step, pitch, length);
    }
}

std::vector<int> voiceChord(int scale, int degree, int key, const std::vector<int>* previous)
{
    int pcs[3];
    chordTones(scale, degree, pcs);
    std::vector<int> cand;
    for (int n = kPadLowest; n <= kPadHighest; ++n) {
        const int pc = ((n - key) % 12 + 12) % 12;
        if (pc == pcs[0] || pc == pcs[1] || pc == pcs[2]) cand.push_back(n);
    }
    static const std::vector<int> kReference = { 58, 63, 67, 72 };
    std::vector<int> best;
    int bestCost = 1 << 30;
    const int c = static_cast<int>(cand.size());
    for (int a = 0; a < c; ++a)
        for (int b = a + 1; b < c; ++b)
            for (int d = b + 1; d < c; ++d)
                for (int e = d + 1; e < c; ++e) {
                    const std::vector<int> v = { cand[static_cast<size_t>(a)], cand[static_cast<size_t>(b)], cand[static_cast<size_t>(d)], cand[static_cast<size_t>(e)] };
                    bool has[3] = {};
                    for (int x : v) for (int k = 0; k < 3; ++k) has[k] = has[k] || ((x - key) % 12 + 12) % 12 == pcs[k];
                    if (!(has[0] && has[1] && has[2])) continue;
                    if (v[1] - v[0] > 12 || v[2] - v[1] > 12 || v[3] - v[2] > 12) continue;   // no holes wider than an octave
                    const int cost = voicingMovement(v, previous != nullptr && previous->size() == 4 ? *previous : kReference);
                    if (cost < bestCost) { bestCost = cost; best = v; }
                }
    return best;
}

int voicingMovement(const std::vector<int>& a, const std::vector<int>& b)
{
    int s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += std::abs(a[i] - b[i]);
    return s;
}

void composeSfxBar(const FormPlan& f, double trackBeat, int barInTrack, std::vector<NoteEvent>& out)
{
    for (const SfxEvent& s : f.sfx) {
        if (static_cast<int>(std::floor(s.beat / kBeatsPerBar)) != barInTrack) continue;
        NoteEvent e;
        e.beat = trackBeat + s.beat;
        e.length = s.length;
        e.part = Part::Sfx;
        e.pitch = static_cast<uint8_t>(kSfxBaseNote + s.type);
        e.velocity = 110;
        out.push_back(e);
    }
}

int bassChordShift(const MelodyPlan& m, int scale, int barInTrack, int bassRoot)
{
    int shift = scaleDegree(scale, m.chordDegree[chordIndexAt(m, barInTrack)]) % 12;
    if (shift > 6 && bassRoot + shift - 12 >= 28) shift -= 12;   // the nearer octave, never under E1
    return shift;
}

} // namespace phos
