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
#include <array>
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
constexpr uint64_t kSaltCounter = 0x434F554E5445000Aull; ///< the counter-lead's material (19.09.2026)
constexpr uint64_t kSaltStab    = 0x535441420000000Bull; ///< the stab's rhythm and material (19.09.2026)
constexpr uint64_t kSaltDrone   = 0x44524F4E4500000Cull; ///< the drone's octave and evolution (19.09.2026)

/** @name Indices of the melodic parts in the arrays kept per part (Form.h, MelodyPart) @{ */
constexpr int kAcidI = mpIndex(MelodyPart::Acid), kLeadI = mpIndex(MelodyPart::Lead), kCounterI = mpIndex(MelodyPart::Counter),
              kArpI = mpIndex(MelodyPart::Arp), kStabI = mpIndex(MelodyPart::Stab), kPadI = mpIndex(MelodyPart::Pad),
              kDroneI = mpIndex(MelodyPart::Drone);
/** @} */

using Allowed = std::vector<std::vector<uint8_t>>;

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

constexpr uint64_t kSaltColour = 0x434F4C4F55520009ull;   ///< the colour slots of a line (18.09.2026)

/**
 * @brief The share of a role's notes that are colour tones, at the arc's full colour (rule 2).
 *
 * The one mechanism for colour since 18.09.2026. The flat second and the upper note of an augmented
 * second are kept out of every sampler set and are placed only at drawn colour slots -- a weak
 * sixteenth followed at once by the tonic -- so the share is what these numbers ask for and nothing
 * the model or the arc could push on top. Before, the arc lifted the colour tones by 1 + 2 * colour
 * in the lead's sets and the neural model's mode table lifted the same tones again; the lead came
 * out at 0.34 to 0.42 in the colourful modes (self test, section "genre rules", before) against the
 * corpus's 0.14 (section "the mode's colour"), and in Phrygian that meant a lead parked on the b2.
 *
 * The values sit a little under the corpus's in-mode shares (acid 0.137, lead 0.141, arp 0.157,
 * measured by the mode-colour section on the three packs) because a colour tone is now always a
 * passing note: the corpus counts every b2, held ones included, and the rules allow none of those.
 * The arp's is the smallest because rule 13 asks for the b2 only as a "short glint". The arc's
 * colour (Composer.cpp: style x arc, 0.82 at the default) scales the target between 0.6 and 1.0 of
 * these values: Farbood's dissonance as one of the quantities an energy arc moves.
 */
constexpr double kColourShare[kNumCorpusRoles] = { 0.11, 0.13, 0.055 };

/** @brief The target colour share of a role at the arc's colour @p colour (0..1). */
double colourTarget(CorpusRoleId role, float colour)
{
    return kColourShare[static_cast<int>(role)] * (0.6 + 0.4 * std::clamp(static_cast<double>(colour), 0.0, 1.0));
}

/** @brief The colour tones of a mode as pitch classes above the tonic (Harmony.h, isColourTone). */
std::vector<int> colourPcs(int scale)
{
    std::vector<int> out;
    for (int d = 0; d < 7; ++d) {
        const int pc = scaleDegree(scale, d) % 12;
        if (isColourTone(scale, pc)) out.push_back(pc);
    }
    return out;
}

/**
 * @brief Allowed symbols with a weight per interval to the part root: @p weight(rel) > 0 admits it.
 * @param tension the measured tension curve's tilt at this position (tensionAt), multiplied on
 *
 * Every weight goes through `quantise`, so no admitted tone can be quantised away. An empty set is
 * returned only when @p weight admits nothing, which the callers never ask for.
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

/** @brief Whether a set admits nothing at all. */
bool emptySet(const std::vector<uint8_t>& a)
{
    for (uint8_t v : a) if (v != 0) return false;
    return true;
}

/**
 * @brief Allowed symbols: the chord's tones in [lo, hi], optionally tilted by the tension curve.
 * @param noColour leave out the chord tones that are colour tones of the mode (rule 1: those are
 *                 neighbour tones and are placed only at colour slots)
 */
std::vector<uint8_t> chordSet(int scale, int degree, int rootOffset, int lo, int hi, double tension = 0.0,
                              bool noColour = false)
{
    int pcs[3];
    chordTones(scale, degree, pcs);
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    std::vector<double> w(static_cast<size_t>(kCorpusAlphabet), 0.0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax) continue;
        const int pc = ((rel + rootOffset) % 12 + 12) % 12;
        if (pc != pcs[0] && pc != pcs[1] && pc != pcs[2]) continue;
        if (noColour && isColourTone(scale, pc)) continue;
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
 * @brief Where a colour tone and its resolution stand: the colour pitch nearest to @p near inside
 *        [lo, hi], and the tonic nearest to it inside [lo, resHi].
 * @return false where the mode has no colour tone or neither fits the window
 */
bool placeColour(int scale, int rootOffset, int lo, int hi, int resHi, int near, Rng& cr, int& colourRel, int& tonicRel)
{
    const std::vector<int> pcs = colourPcs(scale);
    if (pcs.empty()) return false;
    // The sampler's alphabet ends at kCorpusRelMin / kCorpusRelMax: a pitch outside it could not be
    // written into a set (sym() would clamp it onto another pitch -- the arp's glint under a G#3
    // anchor landed on the anchor itself).
    lo = std::max(lo, kCorpusRelMin);
    hi = std::min(hi, kCorpusRelMax);
    resHi = std::min(resHi, kCorpusRelMax);
    // The flat second is the genre's signature interval (Easwaran 2004): twice as likely as the others.
    std::vector<double> w;
    for (int pc : pcs) w.push_back(pc == 1 ? 2.0 : 1.0);
    const int pc = pcs[static_cast<size_t>(drawIndex(cr, w.data(), static_cast<int>(w.size())))];
    int best = 1 << 20;
    for (int rel = lo; rel <= hi; ++rel)
        if ((((rel + rootOffset) % 12) + 12) % 12 == pc && std::abs(rel - near) < std::abs(best - near)) best = rel;
    if (best == (1 << 20)) return false;
    int tonic = 1 << 20;
    for (int rel = lo; rel <= resHi; ++rel)
        if ((((rel + rootOffset) % 12) + 12) % 12 == 0 && std::abs(rel - best) < std::abs(tonic - best)) tonic = rel;
    if (tonic == (1 << 20)) return false;
    colourRel = best;
    tonicRel = tonic;
    return true;
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
    int mode = 0;                ///< the mode of the line, as the section plays it (kScaleSteps). Read
                                 ///< only by a weight file whose header carries @c condMode; every
                                 ///< older file ignores it, which is why the makers may always set it.
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
        NeuralStepper stepper{ src.nn, static_cast<int>(role), src.style, bars, &cond, 0, src.mode };
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

/**
 * @brief Redraws single notes of a drawn line until it keeps @p rules.
 *
 * Each repair redraws one note from the same model, with every other note of the line fixed -- the
 * constrained sampler then draws that note from the model's distribution given both its neighbours,
 * which is how the variations of the acid have always been drawn. The note's own set loses exactly
 * what the rule forbids there (the pitch of a run, or the pitch classes a window already has too
 * many or too few of), so the learned model still chooses; it only chooses inside the rule. The
 * rules win where the model disagrees: the acid corpus plays two pitch classes a bar and repeats
 * 59 % of its notes (docs/PLAN.md, 18.09.2026), and that is exactly what the brake is there for.
 *
 * @param fixed positions no repair may touch (the tonic that opens a cell, colour slots and their
 *              resolutions, the fifth a lead rests on, the notes a variation keeps)
 */
void enforceLine(const PitchModel& model, CorpusRoleId role, const DrawSource& src, const Allowed& allowed,
                 const std::vector<int>& steps, int bars, std::vector<uint8_t> fixed, const LineRules& rules,
                 int ctx2, int ctx1, double temperature, Rng& r, std::vector<int>& rels)
{
    const size_t n = rels.size();
    if (n < 2) return;
    auto pcOf = [](int rel) { return ((rel % 12) + 12) % 12; };
    for (int iter = 0; iter < 96; ++iter) {
        int pos = -1;
        std::vector<uint8_t> set;
        // Runs: the last free note of a run of maxRun + 1 loses the run's pitch.
        const size_t len = rules.loop ? n + static_cast<size_t>(rules.maxRun) : n;
        for (size_t k = static_cast<size_t>(rules.maxRun); k < len && pos < 0; ++k) {
            bool run = true;
            for (int d = 1; d <= rules.maxRun && run; ++d) run = rels[k % n] == rels[(k - static_cast<size_t>(d)) % n];
            if (!run) continue;
            for (int d = 0; d <= rules.maxRun; ++d) {
                const size_t c = (k - static_cast<size_t>(d)) % n;
                if (fixed[c]) continue;
                set = allowed[c];
                set[static_cast<size_t>(sym(rels[c]))] = 0;
                if (emptySet(set)) continue;
                pos = static_cast<int>(c);
                break;
            }
        }
        // Pitch classes per window.
        if (pos < 0 && rules.pcLo > 0) {
            const int windows = std::max(1, (steps.empty() ? 0 : steps.back()) / rules.window + 1);
            for (int w = 0; w < windows && pos < 0; ++w) {
                int count[12] = {};
                std::vector<size_t> idx;
                for (size_t i = 0; i < n; ++i)
                    if (steps[i] / rules.window == w) { idx.push_back(i); ++count[pcOf(rels[i])]; }
                int distinct = 0;
                for (int c : count) distinct += c > 0 ? 1 : 0;
                if (idx.empty() || (distinct >= rules.pcLo && distinct <= rules.pcHi)) continue;
                const bool tooFew = distinct < rules.pcLo;
                // Too few: the commonest class gives a note away to a class the window lacks.
                // Too many: the rarest class's note moves onto a class the window already has.
                std::vector<size_t> order(idx.rbegin(), idx.rend());
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    return tooFew ? count[pcOf(rels[a])] > count[pcOf(rels[b])] : count[pcOf(rels[a])] < count[pcOf(rels[b])];
                });
                for (size_t c : order) {
                    if (fixed[c]) continue;
                    set = allowed[c];
                    for (size_t s = 0; s < set.size(); ++s) {
                        if (!set[s]) continue;
                        const int pc = pcOf(PitchModel::rel(static_cast<int>(s)));
                        const bool keep = tooFew ? count[pc] == 0 : (count[pc] > 0 && pc != pcOf(rels[c]));
                        if (!keep) set[s] = 0;
                    }
                    if (emptySet(set)) continue;
                    pos = static_cast<int>(c);
                    break;
                }
            }
        }
        if (pos < 0) return;
        Allowed one;
        for (size_t i = 0; i < n; ++i) one.push_back(static_cast<int>(i) == pos ? set : single(rels[i]));
        const std::vector<int> redrawn = drawPitches(model, role, src, one, steps, bars, ctx2, ctx1, temperature, r);
        if (redrawn[static_cast<size_t>(pos)] == rels[static_cast<size_t>(pos)]) { fixed[static_cast<size_t>(pos)] = 1; continue; }
        rels = redrawn;
    }
}

/** @brief Whether a line keeps @p rules (the check enforceLine works towards). */
bool lineKeeps(const std::vector<int>& rels, const std::vector<int>& steps, const LineRules& rules)
{
    const size_t n = rels.size();
    if (n >= static_cast<size_t>(rules.maxRun) + 1) {
        const size_t len = rules.loop ? n + static_cast<size_t>(rules.maxRun) : n;
        for (size_t k = static_cast<size_t>(rules.maxRun); k < len; ++k) {
            bool run = true;
            for (int d = 1; d <= rules.maxRun && run; ++d) run = rels[k % n] == rels[(k - static_cast<size_t>(d)) % n];
            if (run) return false;
        }
    }
    if (rules.pcLo > 0 && n > 0) {
        const int windows = steps.back() / rules.window + 1;
        for (int w = 0; w < windows; ++w) {
            bool seen[12] = {};
            int distinct = 0, notes = 0;
            for (size_t i = 0; i < n; ++i) {
                if (steps[i] / rules.window != w) continue;
                ++notes;
                const int pc = ((rels[i] % 12) + 12) % 12;
                if (!seen[pc]) { seen[pc] = true; ++distinct; }
            }
            if (notes > 0 && (distinct < rules.pcLo || distinct > rules.pcHi)) return false;
        }
    }
    return true;
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

namespace {

/**
 * @brief A pendulum: the two degrees a track's core alternates between, and each style's taste for it.
 *
 * The user's brief (22.09.2026): "Wenn Akkorde wechseln, nutzen Produzenten fast immer sogenannte
 * Pendel-Harmonien" -- i <-> bII (the Phrygian pendulum, "die Koenigsklasse"), i <-> bVII (subtonic,
 * driving), i <-> iv (the shamanic fourth), and for the more melodic styles i <-> bVI and i <-> v.
 * A pendulum exists only where the mode has its second degree at the interval the name says:
 * bII wants a semitone, bVII a whole tone under the octave, bVI a minor sixth.
 */
struct Pendulum { int b; int semis; double weight[kNumStyles]; };
const Pendulum kPendulums[] = {
    //  b  semis   Goa   FullOn  Prog   Dark   HiTech
    { 1,  1,  { 0.50, 0.20, 0.00, 0.50, 0.50 } },   // i <-> bII
    { 6, 10,  { 0.30, 0.40, 0.35, 0.15, 0.30 } },   // i <-> bVII
    { 3,  5,  { 0.20, 0.25, 0.35, 0.30, 0.20 } },   // i <-> iv
    { 5,  8,  { 0.00, 0.10, 0.30, 0.20, 0.00 } },   // i <-> bVI
    { 4,  7,  { 0.00, 0.15, 0.00, 0.00, 0.00 } },   // i <-> v
};
constexpr int kNumPendulums = static_cast<int>(sizeof(kPendulums) / sizeof(kPendulums[0]));

/**
 * @brief Each style's taste for the chord types of Harmony.h, in ChordType order:
 *        triad, sus2, sus4, m7, m9, maj7, m(b9), sus(b2), phryg.dom, m(b5), quartal.
 *
 * The mode decides which of them exist (chordTypeFits); this only says what the style reaches for
 * among those that do. m(b5) fits no root in any mode and is a chromatic cluster the two dark styles
 * alone may take (the brief: "Darkpsy, Forest, Twilight").
 */
const double kTypeWeight[kNumStyles][kNumChordTypes] = {
    //  triad sus2  sus4  m7    m9    maj7  m(b9) susb2 hijaz m(b5) quartal
    { 0.15, 0.05, 0.15, 0.05, 0.00, 0.10, 0.30, 0.20, 0.25, 0.00, 0.00 },   // Goa
    { 0.20, 0.25, 0.20, 0.20, 0.05, 0.10, 0.15, 0.05, 0.00, 0.00, 0.05 },   // Full-On
    { 0.05, 0.15, 0.10, 0.30, 0.25, 0.10, 0.00, 0.00, 0.00, 0.00, 0.20 },   // Progressive
    { 0.10, 0.05, 0.05, 0.05, 0.00, 0.05, 0.30, 0.25, 0.05, 0.20, 0.15 },   // Dark Forest
    { 0.05, 0.20, 0.05, 0.00, 0.00, 0.05, 0.15, 0.30, 0.00, 0.25, 0.25 },   // Hi-Tech
};

/** @brief How long a chord holds in the cores: 4, 8 or 16 bars, by style ("oft nur alle 4, 8 oder 16 Takte"). */
const double kBarsWeight[kNumStyles][3] = {
    { 0.20, 0.50, 0.30 },   // Goa
    { 0.40, 0.50, 0.10 },   // Full-On
    { 0.00, 0.40, 0.60 },   // Progressive
    { 0.00, 0.50, 0.50 },   // Dark Forest
    { 0.50, 0.50, 0.00 },   // Hi-Tech
};

/** @brief Draws a chord type for @p degree in @p scale from the style's taste, among the types that fit. */
/**
 * @brief The classical taste (23.09.2026, round "Harmonie"): triads and sevenths first, the suspensions
 *        after them, the colour types hardly -- what a loop progression is voiced with, so that a bVI or
 *        a bVII carries its third and the loop sounds like the progression it is rather than "schraeg".
 */
const double kClassicalWeight[kNumChordTypes] = {
    //  triad sus2  sus4  m7    m9    maj7  m(b9) susb2 hijaz m(b5) quartal
    0.50, 0.12, 0.08, 0.20, 0.05, 0.20, 0.02, 0.02, 0.02, 0.00, 0.03,
};

int drawChordType(Rng& r, int scale, int degree, int styleIdx, bool classical = false)
{
    const bool dark = styleIdx == static_cast<int>(StyleId::DarkForest) || styleIdx == static_cast<int>(StyleId::HiTech);
    double w[kNumChordTypes] = {};
    double sum = 0.0;
    for (int t = 0; t < kNumChordTypes; ++t) {
        const ChordType ct = static_cast<ChordType>(t);
        const bool fits = ct == ChordType::MinFlat5 ? (dark && !classical) : chordTypeFits(scale, degree, ct);
        w[t] = fits ? (classical ? kClassicalWeight[t] : kTypeWeight[styleIdx][t]) : 0.0;
        sum += w[t];
    }
    if (sum <= 0.0) {
        // Nothing the style likes fits: the first type that fits at all, and the triad only when
        // even that is wanting -- a chord must never hand the pad a note outside its mode.
        for (int t = 0; t < kNumChordTypes; ++t) if (chordTypeFits(scale, degree, static_cast<ChordType>(t))) return t;
        return static_cast<int>(ChordType::Triad);
    }
    return drawIndex(r, w, kNumChordTypes);
}

} // namespace

/**
 * @brief The minor loops a track may play instead of the pendulum (23.09.2026, round "Harmonie").
 *
 * The user found the ban on classical harmony too strict ("sonst klingt immer alles irgendwie schraeg").
 * The literature keeps psytrance modal at heart but grants the progressive and full-on branches short
 * minor loops (KVR, "Goa/Psytrance and music theory"; Outerverse, "Scales & modes in psytrance"): the
 * aeolian three, its turn, the plagal-dominant swing, and the descending four. Each names its degrees
 * over the four slots and what pitch class each degree has to be for the loop to exist in a mode.
 */
struct Loop { int deg[4]; int semis[4]; const char* name; };
const Loop kLoops[4] = {
    { { 0, 5, 6, 0 }, { 0, 8, 10, 0 },  "i-bVI-bVII-i" },
    { { 0, 6, 5, 6 }, { 0, 10, 8, 10 }, "i-bVII-bVI-bVII" },
    { { 0, 3, 0, 4 }, { 0, 5, 0, 7 },   "i-iv-i-v" },
    { { 0, 2, 6, 3 }, { 0, 3, 10, 5 },  "i-bIII-bVII-iv" },
};
constexpr int kNumLoops = 4;
/** @brief How often a style plays a loop instead of a pendulum, and how often the second half changes. */
const float kLoopChance[kNumStyles] = { 0.20f, 0.35f, 0.45f, 0.15f, 0.10f };      // Goa, Full-On, Progressive, Dark, Hi-Tech
const float kSecondHalfChance[kNumStyles] = { 0.30f, 0.30f, 0.40f, 0.25f, 0.20f };

/** @brief Whether every degree of @p loop exists in @p scale with the loop's pitch class and a perfect fifth over it. */
bool loopFits(int scale, const Loop& loop)
{
    for (int c = 0; c < 4; ++c) {
        const int d = loop.deg[c];
        if (scaleDegree(scale, d) % 12 != loop.semis[c]) return false;
        if (scaleDegree(scale, d + 4) - scaleDegree(scale, d) != 7) return false;
    }
    return true;
}

/**
 * @brief Draws one progression -- a pendulum or a loop -- into @p deg / @p type; returns 1 for a loop, 0 for
 *        a pendulum. The loop's coin and its choice come from @p lr, the pendulum from @p r as before, so a
 *        track that draws the pendulum plays exactly what it played before this round.
 */
int drawProgression(Rng& r, Rng& lr, int scale, int si, int* deg, int* type, int& which)
{
    which = -1;
    double lw[kNumLoops] = {};
    double lsum = 0.0;
    for (int i = 0; i < kNumLoops; ++i) { lw[i] = loopFits(scale, kLoops[i]) ? 1.0 : 0.0; lsum += lw[i]; }
    const bool loop = lr.uniform() < kLoopChance[si] && lsum > 0.0;
    const int pick = drawIndex(lr, lw, kNumLoops);   // drawn whatever the coin: the pendulum path keeps its stream
    if (loop) {
        which = pick;
        for (int c = 0; c < 4; ++c) { deg[c] = kLoops[pick].deg[c]; type[c] = drawChordType(lr, scale, deg[c], si, true); }
        // The tonic's two slots take one type, as the pendulum's do.
        type[3] = kLoops[pick].deg[3] == 0 ? type[0] : type[3];
        type[2] = kLoops[pick].deg[2] == 0 ? type[0] : type[2];
        return 1;
    }
    double pw[kNumPendulums] = {};
    double psum = 0.0;
    for (int i = 0; i < kNumPendulums; ++i) {
        const Pendulum& pd = kPendulums[i];
        if (scaleDegree(scale, pd.b) % 12 != pd.semis) continue;
        if (scaleDegree(scale, pd.b + 4) - scaleDegree(scale, pd.b) != 7) continue;
        pw[i] = pd.weight[si];
        psum += pw[i];
    }
    const int other = psum > 0.0 ? kPendulums[drawIndex(r, pw, kNumPendulums)].b : 3;   // iv exists in every mode
    deg[0] = 0; deg[1] = other; deg[2] = 0; deg[3] = other;
    const int typeI = drawChordType(r, scale, 0, si), typeOther = drawChordType(r, scale, other, si);
    type[0] = typeI; type[1] = typeOther; type[2] = typeI; type[3] = typeOther;
    return 0;
}

void makeChords(MelodyPlan& m, int scale, uint64_t seed, double temperature, const StyleProfile& style)
{
    (void)temperature;   // the corpus successions this used to temper are no longer the rule (22.09.2026)
    Rng r;
    r.seed(seed ^ kSaltChords);
    const int si = std::clamp(static_cast<int>(style.id), 0, kNumStyles - 1);
    Rng lr;   // the loops' and the second half's own stream (23.09.2026): nothing older moves because of them
    lr.seed(mixSeed(seed ^ kSaltChords, 0x4C4F4F50ull));

    // The pendulum: among those the mode has, by the style's taste. The corpus successions
    // (chordTransition) decided this until 22.09.2026 and gave four degrees every 2 or 4 bars --
    // cadences, in effect, which is what the brief says a psytrance pad does not play. Rules over
    // corpus (the user's standing rule of 18.09.2026): the corpus fills in what the rules leave open,
    // and here they leave nothing open.
    // (The pendulum's chord has to stand on a perfect fifth: v in Phrygian is C# over F#, and its scale
    // fifth is G, a tritone -- no type of Harmony.h fits such a root and the pad would be handed a note
    // outside the mode; measured on seed 7, track 3. drawProgression keeps that rule for loops as well.)
    m.progression = drawProgression(r, lr, scale, si, m.chordDegree, m.chordType, m.loop);
    const int typeI = m.chordType[0];
    static const int kBars[3] = { 4, 8, 16 };
    m.chordBars = kBars[drawIndex(r, kBarsWeight[si], 3)];
    // The second half (23.09.2026): behind the main breakdown a progression of its own, in the style's
    // share of tracks -- another pendulum or a loop, never the first one again. The lines and the bass
    // stay on the tonic (the Bordun), so this is the pad's and the stab's colour changing, not a key.
    m.secondHalf = lr.uniform() < kSecondHalfChance[si];
    if (m.secondHalf) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            m.progression2 = drawProgression(lr, lr, scale, si, m.chordDegree2, m.chordType2, m.loop2);
            bool same = m.progression2 == m.progression;
            for (int c = 0; c < 4 && same; ++c) same = m.chordDegree2[c] == m.chordDegree[c];
            if (!same) break;
            if (attempt == 3) m.secondHalf = false;
        }
    }

    // The main breakdown: the aeolian three (i - bVI - bVII) where the mode has both -- "die dramatischste
    // und melodischste Progression im Psytrance, fast ausschliesslich im Main Breakdown" -- else one
    // chord held through it, the most emotional type the mode allows (m9, m7, else the tonic's own).
    const bool hasVI = scaleDegree(scale, 5) % 12 == 8, hasVII = scaleDegree(scale, 6) % 12 == 10;
    if (hasVI && hasVII) {
        m.breakHolds = false;
        m.breakChordBars = 8;
        const int deg[4] = { 0, 5, 6, 0 };
        for (int c = 0; c < 4; ++c) { m.breakDegree[c] = deg[c]; m.breakType[c] = drawChordType(r, scale, deg[c], si); }
    } else {
        m.breakHolds = true;
        m.breakChordBars = 16;
        int t = typeI;
        if (chordTypeFits(scale, 0, ChordType::Min9)) t = static_cast<int>(ChordType::Min9);
        else if (chordTypeFits(scale, 0, ChordType::Min7)) t = static_cast<int>(ChordType::Min7);
        for (int c = 0; c < 4; ++c) { m.breakDegree[c] = 0; m.breakType[c] = t; }
    }
}

} // namespace (padChordAt has external linkage: the header declares it)

PadChord padChordAt(const MelodyPlan& m, const BarPlan& bp, int barInTrack)
{
    PadChord c;
    if (bp.type == SectionType::Break) {
        c.inBreak = true;
        if (bp.mainBreak && !m.breakHolds) {
            c.blockBars = m.breakChordBars;
            c.slot = (bp.barInSection / c.blockBars) % 4;
            c.barInBlock = bp.barInSection % c.blockBars;
        } else {
            // Held for the whole section: one chord from its first bar to its last.
            c.blockBars = std::max(1, bp.sectionBars);
            c.slot = 0;
            c.barInBlock = bp.barInSection;
        }
        c.degree = m.breakDegree[c.slot];
        c.type = m.breakType[c.slot];
        return c;
    }
    c.blockBars = m.chordBars;
    c.slot = chordIndexAt(m, barInTrack);
    c.barInBlock = barInTrack % m.chordBars;
    c.secondSet = m.secondHalf && bp.afterMainBreak;
    c.degree = c.secondSet ? m.chordDegree2[c.slot] : m.chordDegree[c.slot];
    c.type = c.secondSet ? m.chordType2[c.slot] : m.chordType[c.slot];
    return c;
}

PadChord padChordAt(const MelodyPlan& m, const FormPlan& f, int barInTrack)
{
    // The same answer from the form alone, for tests and tools that have no BarPlan in hand: the four
    // fields padChordAt reads are the section's type, where in it the bar lies, how long it is, and
    // whether it is the breakdown before the climax drop.
    BarPlan bp;
    const int si = sectionOfBar(f, barInTrack);
    const Section& s = f.section[std::clamp(si, 0, std::max(0, f.count - 1))];
    bp.type = s.type;
    bp.barInSection = barInTrack - s.startBar;
    bp.sectionBars = s.bars;
    if (s.type == SectionType::Break)
        for (int j = si + 1; j < f.count; ++j) {
            if (f.section[j].type != SectionType::Drop) continue;
            bp.mainBreak = f.section[j].climax;
            break;
        }
    for (int k = 0; k < si; ++k) {
        if (f.section[k].type != SectionType::Break) continue;
        for (int j = k + 1; j < f.count; ++j) {
            if (f.section[j].type != SectionType::Drop) continue;
            if (f.section[j].climax) bp.afterMainBreak = true;
            break;
        }
    }
    return padChordAt(m, bp, barInTrack);
}

namespace {

/**
 * @brief The acid line: a one- or two-bar cell inside the TB-303 rules of 18.09.2026 (Melody.h).
 *
 * **Rhythm first, and dense** (rules 3 and 5). Each bar gets two to five sixteenths without an
 * onset -- 11 to 14 onsets -- split evenly between the halves of the bar, never two side by side and
 * never the downbeat. Up to three of them are rests, kick steps first; the rest are ties that hold
 * the note before. Which steps they are is weighted by how often the corpus leaves each step empty,
 * so the learned rhythm still chooses; it only chooses inside the density. The old draw followed the
 * corpus's onset chain freely and came out at 5.4 onsets a bar, 29 % of the cells front-loaded.
 *
 * **Accents** (rule 8) on the "e" and the "a" of each beat, never on a kick step, clustering with the
 * lift measured in the corpus (below). **Slides** (rule 9) into any note that follows within two
 * sixteenths without a rest between, with probability 0.4 (0.2 before), the last note of the cell
 * included -- it slides across the barline into the cell's first note.
 *
 * **Pitches** (rules 1, 6, 7, 10): the tonic opens the cell; held notes rest on the tonic or the
 * fifth; every other note is drawn by the corpus model from 1, b3 (or 3), 5 and b7 (or 7), with the
 * fourth and the sixth as rarer passing notes, inside D3..D4 -- plus, on offbeat sixteenths, the
 * tonic, the fifth and the seventh an octave up, as far as D5. Colour tones stand only at colour
 * slots, resolving to the tonic. Then enforceLine keeps three to five pitch classes a bar and no
 * pitch three times in a row.
 *
 * **Variation** (rule 4): A' redraws one or two notes of A, A'' one or two of A' in its last beat;
 * the second set (after the first breakdown) redraws a third of A's free notes and varies that the
 * same way. The first two notes are never redrawn, so a run cannot form across a cell boundary.
 */
void makeAcid(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src)
{
    src.mode = std::clamp(scale, 0, kNumScales - 1);   // what the section plays, for a model that asks
    Rng r;
    r.seed(seed ^ kSaltAcid);
    Rng cr;
    cr.seed(seed ^ kSaltColour ^ 1u);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Acid)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Acid);
    m.acidSteps = r.uniform() < 0.7f ? 16 : 32;
    const int steps = m.acidSteps;
    const int bars = steps / 16;
    const int root = m.root[kAcidI];
    const int rootOffset = ((root - key) % 12 + 12) % 12;

    // ---- rhythm: 0 onset, 1 tie (the note before holds), 2 rest
    std::vector<uint8_t> kind(static_cast<size_t>(steps), 0);
    for (int b = 0; b < bars; ++b) {
        const int off = 2 + r.below(4);                    // 11..14 onsets
        const int rests = std::min(off, 1 + r.below(3));   // at most three deliberate rests
        const int half1 = off / 2 + ((off % 2) != 0 && r.uniform() < 0.5f ? 1 : 0);
        std::vector<int> chosen;
        for (int h = 0; h < 2; ++h) {
            double w[16] = {};
            for (int s = h * 8; s < h * 8 + 8; ++s) {
                if (s == 0) continue;   // the bar's downbeat always sounds
                const auto& c = role.onset[s];
                const double total = static_cast<double>(c[0][0]) + c[0][1] + c[1][0] + c[1][1];
                const double pOff = total > 0.0 ? (static_cast<double>(c[0][0]) + c[1][0]) / total : 0.3;
                w[s] = (0.05 + pOff) * (s % 4 == 0 ? 2.5 : 1.0);   // kick steps are where a 303 rests
            }
            const int want = h == 0 ? half1 : off - half1;
            for (int k = 0; k < want; ++k) {
                double any = 0.0;
                for (double v : w) any += v;
                if (any <= 0.0) break;
                const int s = drawIndex(r, w, 16);
                chosen.push_back(s);
                w[s] = 0.0;
                if (s > 0) w[s - 1] = 0.0;
                if (s < 15) w[s + 1] = 0.0;
            }
        }
        std::stable_sort(chosen.begin(), chosen.end(), [](int a, int c) { return (a % 4 == 0) > (c % 4 == 0); });
        for (size_t k = 0; k < chosen.size(); ++k)
            kind[static_cast<size_t>(b * 16 + chosen[k])] = static_cast<uint8_t>(static_cast<int>(k) < rests ? 2 : 1);
    }
    std::vector<int> on, lens;
    for (int s = 0; s < steps; ++s) {
        if (kind[static_cast<size_t>(s)] != 0) continue;
        int l = 1;
        while (s + l < steps && kind[static_cast<size_t>(s + l)] == 1) ++l;
        on.push_back(s);
        lens.push_back(l);
    }
    const size_t n = on.size();

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
     * accents, and the lift rests on the nine that do.
     *
     * The chain keeps the marginal accent rate of each step position where it is asked to be. Since
     * 18.09.2026 the rates follow rule 8 -- 0.36 on the "e" and the "a" of the beat, 0.06 on the "and",
     * none on the beat itself, where the kick is (before: 0.12 on the beat, 0.32 on the "and", 0.2
     * elsewhere) -- and with rates that differ this much from one onset to the next the chain has to
     * know the rate of the onset before: for a target m at this position, a rate m' at the previous
     * onset and the lift L, P(accent) = x (1 + (L - 1) m') must equal m, so the probability after a
     * plain note is x = m / (1 + (L - 1) m') and after an accent L x. Where m' = m this is the
     * m / (1 - m + L m) of before. Without the correction the "e" and "a" came out at 0.24 instead of
     * 0.36, because the onset before them is mostly a kick step that is never accented.
     */
    constexpr float kAccentLift = 2.51f;
    auto accentRate = [](int step) { const int pos = step % 4; return (pos == 1 || pos == 3) ? 0.36f : (pos == 2 ? 0.06f : 0.0f); };
    std::vector<uint8_t> flags(n, 0);
    bool prevAccent = false;
    for (size_t i = 0; i < n; ++i) {
        const float pAccent = accentRate(on[i]);
        const float prevRate = accentRate(on[i == 0 ? n - 1 : i - 1]);
        const float pPlain = pAccent / (1.0f + (kAccentLift - 1.0f) * prevRate);
        prevAccent = pAccent > 0.0f && r.uniform() < (prevAccent ? std::min(1.0f, kAccentLift * pPlain) : pPlain);
        if (prevAccent) flags[i] |= kNoteAccent;
        const bool last = i + 1 == n;
        const int gap = (last ? steps + on[0] : on[i + 1]) - on[i];
        if (gap <= 2 && lens[i] == gap && r.uniform() < 0.4f) flags[i] |= kNoteSlide;
    }
    // A' moves one accent from one "e"/"a" to another (drawn now, before any pitch, so that a borrowed
    // mode keeps the rhythm, the accents and the slides of the track's own).
    int accentFrom = -1, accentTo = -1;
    {
        std::vector<int> acc, plain;
        for (size_t i = 0; i < n; ++i)
            if (on[i] % 2 == 1) ((flags[i] & kNoteAccent) ? acc : plain).push_back(static_cast<int>(i));
        if (!acc.empty() && !plain.empty()) {
            accentFrom = acc[static_cast<size_t>(r.below(static_cast<int>(acc.size())))];
            accentTo = plain[static_cast<size_t>(r.below(static_cast<int>(plain.size())))];
        }
    }

    // ---- pitches
    const int lo = kAcidLowest - root, hi = kAcidHighest - root, jumpHi = kAcidJumpHighest - root;
    double core[12] = {}, rest[12] = {}, jump[12] = {};
    const struct { int degree; double w; } kCore[] = { { 0, 6.0 }, { 2, 3.0 }, { 4, 4.0 }, { 6, 3.0 }, { 3, 1.0 }, { 5, 1.0 } };
    for (const auto& c : kCore) core[scaleDegree(scale, c.degree) % 12] = c.w;
    rest[0] = 6.0;
    rest[7] = 4.0;
    jump[0] = jump[7] = 1.0;
    jump[scaleDegree(scale, 6) % 12] = 0.6;
    for (int pc = 0; pc < 12; ++pc)
        if (isColourTone(scale, pc)) core[pc] = rest[pc] = jump[pc] = 0.0;
    auto pcOf = [&](int rel) { return ((rel + rootOffset) % 12 + 12) % 12; };
    Allowed allowed;
    for (size_t i = 0; i < n; ++i) {
        const double t = tensionAt(CorpusRoleId::Acid, on[i], bars);
        const bool odd = on[i] % 2 == 1;
        if (i == 0) allowed.push_back(single(0));
        else if (lens[i] >= 2) allowed.push_back(weightedSet(rootOffset, lo, hi, [&](int rel) { return rest[pcOf(rel)]; }, t));
        else allowed.push_back(weightedSet(rootOffset, lo, odd ? jumpHi : hi, [&](int rel) {
            // Octave jumps are occasional: a quarter of the weight a note in the register gets.
            return rel <= hi ? core[pcOf(rel)] : 0.25 * jump[pcOf(rel)];
        }, t));
    }
    std::vector<uint8_t> fixed(n, 0);
    fixed[0] = 1;
    {
        std::vector<uint8_t> taken(n, 0);
        taken[0] = 1;
        const std::vector<int> slots = colourSlots(on, lens, taken, true, steps, colourTarget(CorpusRoleId::Acid, colour), cr,
                                                   [&](int i) { return i == 0 || !emptySet(allowed[static_cast<size_t>(i)]); });
        for (int i : slots) {
            const size_t nx = (static_cast<size_t>(i) + 1) % n;
            int c = 0, t = 0;
            if (!placeColour(scale, rootOffset, lo, hi, nx == 0 ? 0 : jumpHi, 0, cr, c, t)) continue;
            if (nx == 0 && t != 0) continue;
            allowed[static_cast<size_t>(i)] = single(c);
            allowed[nx] = single(t);
            fixed[static_cast<size_t>(i)] = fixed[nx] = 1;
        }
    }
    LineRules rules;
    rules.loop = true;
    rules.pcLo = 3;
    rules.pcHi = 5;
    // The draw and its repair, repeated where a repair got stuck (a note whose set the rules empty):
    // a fresh draw is a fresh start, and eight of them have never all failed in the self test.
    std::vector<int> base;
    for (int attempt = 0; attempt < 8; ++attempt) {
        base = drawPitches(model, CorpusRoleId::Acid, src, allowed, on, bars, 0, 0, temperature, r);
        enforceLine(model, CorpusRoleId::Acid, src, allowed, on, bars, fixed, rules, 0, 0, temperature, r, base);
        if (lineKeeps(base, on, rules)) break;
    }

    // A variation: `changes` free notes (from step `fromStep` on) redrawn, each forbidden its old pitch.
    // Tried again with other notes where the rules cannot be kept with the ones drawn first. The first
    // two notes are never redrawn, so a run cannot form across the boundary between two cells.
    std::vector<uint8_t> held = fixed;
    if (n > 1) held[1] = 1;
    auto vary = [&](const std::vector<int>& from, int changes, int fromStep) {
        std::vector<int> out;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::vector<int> cand;
            for (size_t i = 0; i < n; ++i) if (!held[i] && on[i] >= fromStep) cand.push_back(static_cast<int>(i));
            if (cand.empty() || attempt >= 4) {
                cand.clear();
                for (size_t i = 0; i < n; ++i) if (!held[i]) cand.push_back(static_cast<int>(i));
            }
            std::vector<uint8_t> keep(n, 1);
            Allowed al;
            for (size_t i = 0; i < n; ++i) al.push_back(single(from[i]));
            for (int c = 0; c < changes && !cand.empty(); ++c) {
                const int k = r.below(static_cast<int>(cand.size()));
                const size_t i = static_cast<size_t>(cand[static_cast<size_t>(k)]);
                cand.erase(cand.begin() + k);
                std::vector<uint8_t> set = allowed[i];
                set[static_cast<size_t>(sym(from[i]))] = 0;
                if (emptySet(set)) { --c; continue; }
                al[i] = set;
                keep[i] = 0;
            }
            out = drawPitches(model, CorpusRoleId::Acid, src, al, on, bars, 0, 0, temperature, r);
            enforceLine(model, CorpusRoleId::Acid, src, al, on, bars, keep, rules, 0, 0, temperature, r, out);
            if (lineKeeps(out, on, rules) && out != from) break;
        }
        return out;
    };
    std::vector<int> cells[kAcidCells];
    cells[acidCell(0, 0)] = base;
    cells[acidCell(1, 0)] = vary(base, std::max(2, static_cast<int>(n) / 3), 0);
    for (int set = 0; set < kMaterialSets; ++set) {
        cells[acidCell(set, 1)] = vary(cells[acidCell(set, 0)], 1 + r.below(2), 0);
        cells[acidCell(set, 2)] = vary(cells[acidCell(set, 1)], 1 + r.below(2), steps - 4);
    }
    for (int k = 0; k < kAcidCells; ++k) {
        std::vector<MelodyNote>& a = m.acid[k];
        a.clear();
        const int variant = k % kAcidVariants;
        for (size_t i = 0; i < n; ++i) {
            MelodyNote note;
            note.step = static_cast<int16_t>(on[i]);
            note.rel = static_cast<int8_t>(cells[k][i]);
            note.flags = flags[i];
            if (variant > 0 && accentFrom >= 0) {
                if (static_cast<int>(i) == accentFrom) note.flags = static_cast<uint8_t>(note.flags & ~kNoteAccent);
                if (static_cast<int>(i) == accentTo) note.flags = static_cast<uint8_t>(note.flags | kNoteAccent);
            }
            // A slide lasts to the next note; everything else is the 303's own gate (a tie holds).
            const bool last = i + 1 == n;
            note.len = static_cast<int16_t>((note.flags & kNoteSlide) ? (last ? steps + on[0] : on[i + 1]) - on[i] : lens[i]);
            // A colour tone is never held: one sixteenth, whatever it follows (rule 1).
            if (isColourTone(scale, pcOf(note.rel))) { note.len = 1; note.flags = static_cast<uint8_t>(note.flags & ~kNoteSlide); }
            note.velocity = (note.flags & kNoteAccent) ? 120 : 88;
            a.push_back(note);
        }
    }
}

/** @brief The chord degree at a phrase step of lead window @p w. */
int chordAtStep(const MelodyPlan& m, int window, int step)
{
    return m.chordDegree[chordIndexAt(m, window * 8 + step / 16)];
}

/** @name The lead's register (rule 17): a soft centre between A4 and C5, a hard ceiling at A5. @{ */
// 20.09.2026, round "dialogue": the centre and the width follow the rule's window C4 .. G4 (Melody.h).
// A sigma of 5 semitones was flat across a window of seven and would have left the register weight with
// nothing to say; at 2.5 the weight falls to 0.6 at the window's edges, which is the same shape the
// wider window had.
/**
 * @brief The counter-lead's gate, as a fraction of its written span (20.09.2026, round "dialogue").
 *
 * The rule asks for "trockenes Staccato" against the lead's legato. At 145 BPM a sixteenth is 103 ms,
 * so 0.45 of at most two sixteenths is a note of at most 93 ms -- shorter than the counter's own filter
 * decay (60 ms) plus its release (35 ms) takes to die, which is what makes it read as a whip rather
 * than as a short held note. The lead's gate is 0.92 of its written length (composeMelodyBar) and its
 * longest written note is two sixteenths, so the measured contrast is a factor of two in note length
 * at the same written span -- and far more where the counter's block gave it four or six sixteenths
 * to hold, which is where it used to sustain over the lead's riff.
 */
constexpr double kCounterStaccato = 0.45;
constexpr double kLeadCentre = 63.5;   ///< MIDI: the middle of C4 .. G4, between D#4 and E4
constexpr double kLeadSigma = 2.5;     ///< semitones: the register weight falls to 0.6 at the window's edges
/** @} */

/**
 * @brief The lead's rhythm over a two-bar motif: a dense figure, not a scatter of notes (rule 16).
 *
 * Three families, drawn once per track so that the motif and B share a character:
 *  - 0, *sixteenths*: every sixteenth, with two to four ties per bar (never on a strong step, never
 *    side by side) -- the rolling Goa lead, 12 to 14 onsets a bar;
 *  - 1, *eighths with pickups*: every eighth, with two to four sixteenth pickups -- 10 to 12;
 *  - 2, *gallop*: a one-beat cell (x.xx, xx.x or xxx.) on every beat, one beat per bar opened to four
 *    sixteenths -- 12 or 13.
 * The second bar repeats the first and redraws only its last beat: the figure is clear because it
 * repeats (rule 4), and the last beat turns the motif round. Which steps get ties or pickups is
 * weighted by the corpus's onset counts, so the learned rhythm chooses inside the density.
 */
std::vector<int> leadRhythm(Rng& r, const CorpusRole& role, int family)
{
    auto pOn = [&](int s) {
        const auto& c = role.onset[s];
        const double total = static_cast<double>(c[0][0]) + c[0][1] + c[1][0] + c[1][1];
        return total > 0.0 ? (static_cast<double>(c[0][1]) + c[1][1]) / total : 0.5;
    };
    static const bool kGallop[3][4] = { { true, false, true, true }, { true, true, false, true }, { true, true, true, false } };
    const int cell = r.below(3);
    bool on[32] = {};
    // Fills steps [from, 16) of the first bar with the family's figure.
    auto fill = [&](int from) {
        const int span = 16 - from;
        if (family == 0) {
            for (int s = from; s < 16; ++s) on[s] = true;
            int ties = span >= 8 ? 2 + r.below(3) : r.below(2);
            double w[16] = {};
            for (int s = std::max(from, 1); s < 16; ++s) w[s] = s % 8 == 0 ? 0.0 : 1.05 - pOn(s);
            while (ties-- > 0) {
                double any = 0.0;
                for (double v : w) any += v;
                if (any <= 0.0) break;
                const int s = drawIndex(r, w, 16);
                on[s] = false;
                w[s] = 0.0;
                if (s > 0) w[s - 1] = 0.0;
                if (s < 15) w[s + 1] = 0.0;
            }
        } else if (family == 1) {
            for (int s = from; s < 16; ++s) on[s] = s % 2 == 0;
            int picks = span >= 8 ? 2 + r.below(3) : 1 + r.below(2);
            double w[16] = {};
            for (int s = from; s < 16; ++s) w[s] = s % 2 == 1 ? 0.05 + pOn(s) : 0.0;
            while (picks-- > 0) {
                double any = 0.0;
                for (double v : w) any += v;
                if (any <= 0.0) break;
                const int s = drawIndex(r, w, 16);
                on[s] = true;
                w[s] = 0.0;
            }
        } else {
            for (int s = from; s < 16; ++s) on[s] = kGallop[cell][s % 4];
            const int firstBeat = from / 4;
            if (firstBeat < 4) {
                const int open = firstBeat == 0 ? 1 + r.below(3) : firstBeat + r.below(4 - firstBeat);
                for (int s = open * 4; s < open * 4 + 4; ++s) if (s >= from) on[s] = true;
            }
        }
    };
    fill(0);
    for (int s = 0; s < 16; ++s) on[16 + s] = on[s];
    // The second bar's last beat, redrawn with the same family: the turn of the motif.
    const bool keep[16] = { on[0], on[1], on[2], on[3], on[4], on[5], on[6], on[7], on[8], on[9], on[10], on[11], on[12], on[13], on[14], on[15] };
    fill(12);
    for (int s = 12; s < 16; ++s) { on[16 + s] = on[s]; on[s] = keep[s]; }
    std::vector<int> out;
    for (int s = 0; s < 32; ++s) if (on[s]) out.push_back(s);
    return out;
}

/**
 * @name The lead's design (22.09.2026, round "Lead")
 *
 * The user's two briefs on the lead ("hören sich eher zufällig an"): a phrase is not eight bars of
 * sampling but one *cell* and an operator program; contour lives on the macro level; the
 * constraints are one weight table over degree stability and metric weight; a critic chooses among
 * candidates; the lead's rhythm interlocks with the bass; and half of what a psytrance lead is, is
 * modulation -- accent, slide, gate. Styles are vectors over the same weights (Form.h, LeadStyle).
 * @{ */

/** @brief An archetype: the cell's transposition per bar in scale steps, and the phrase's filter arc. */
struct LeadArchetypeDef { int8_t step[8]; float arc[8]; };

/**
 * @brief The five archetypes of the brief, as bar offsets and cutoff arcs.
 *
 * The offsets are scale steps the cell is transposed by, and each row sums to zero: the contour is a
 * *shape* around the cell's register, not a lift out of it. Written as lifts (0 0 0 0 1 1 2 0 for the
 * surge) the raised bars carried more than half the phrase's notes and the listening seed's median
 * left the register rule's lower half (Melody.h; measured, G#4 against the F#4 bound). Pedal & Bounce
 * never moves -- its movement is the operators'. The cadence bar lands on the tonic whatever its
 * offset (CellOp::EndCadence). The arcs are what Composer.cpp writes onto the lead's cutoff, bar by
 * bar, around the section's own value (leadArc).
 */
const LeadArchetypeDef kLeadArchetypes[kNumLeadArchetypes] = {
    { { -1, -1, 0, 0, 0, 1, 2, -1 },  { 0.10f, 0.15f, 0.20f, 0.30f, 0.45f, 0.60f, 0.85f, 0.30f } },   // Phrygian Surge: flat, a slow rise to the peak in bar 7
    { { -1, 0, 0, 1, 1, 0, 0, -1 },   { 0.20f, 0.35f, 0.55f, 0.75f, 0.75f, 0.55f, 0.35f, 0.20f } },   // Arch & Drop: up over the first half, down over the second
    { { 0, 0, 0, 0, 0, 0, 0, 0 },     { 0.30f, 0.30f, 0.30f, 0.45f, 0.30f, 0.30f, 0.30f, 0.55f } },   // Pedal & Bounce
    { { 1, 1, 1, 0, 0, -1, -1, -1 },  { 0.80f, 0.70f, 0.60f, 0.50f, 0.40f, 0.30f, 0.20f, 0.30f } },   // Descending Cascade
    { { -1, 0, -1, 0, 0, 1, 1, 0 },   { 0.30f, 0.50f, 0.30f, 0.50f, 0.50f, 0.70f, 0.85f, 0.30f } },   // Tension Call: calls that rise and do not resolve until the end
};

/** @brief Semitones over the tonic of scale degree @p d of @p scale, for any integer @p d. */
int degreeRel(int scale, int d)
{
    if (d >= 0) return scaleDegree(scale, d);
    const int k = (-d + 6) / 7;
    return scaleDegree(scale, d + 7 * k) - 12 * k;
}

/** @brief The degree (0..6) whose pitch class is @p pc in @p scale, -1 for a chromatic class. */
int degreeOfPc(int scale, int pc)
{
    for (int d = 0; d < 7; ++d) if (scaleDegree(scale, d) % 12 == ((pc % 12) + 12) % 12) return d;
    return -1;
}

/**
 * @brief @p rel (semitones over the part root) moved by @p steps scale degrees.
 *
 * A chromatic pitch -- none should reach here, the cell keeps colour tones out -- moves with the
 * degree just under it.
 */
int transposeRel(int scale, int rootOffset, int rel, int steps)
{
    if (steps == 0) return rel;
    const int t = rel + rootOffset;                      // semitones over the tonic
    const int pc = ((t % 12) + 12) % 12;
    int d = degreeOfPc(scale, pc);
    int chroma = 0;
    if (d < 0) {
        d = 0;
        for (int k = 0; k < 7; ++k) if (scaleDegree(scale, k) % 12 < pc) d = k;
        chroma = pc - scaleDegree(scale, d) % 12;
    }
    const int oct = (t - pc) / 12;                       // exact: t - pc is a multiple of 12
    return degreeRel(scale, 7 * oct + d + steps) + chroma - rootOffset;
}

/**
 * @brief The metric weight of a sixteenth for the stability table: how hard the position pulls a
 *        note to the stable degrees. Negative on the odd sixteenths, where the brief *wants* the
 *        tension degrees ("Synkopen dürfen (sollen!) Spannungsstufen tragen").
 */
double metricWeight(int step)
{
    const int s = ((step % 16) + 16) % 16;
    if (s == 0) return 1.0;
    if (s == 8) return 0.8;
    if (s % 4 == 0) return 0.5;
    if (s % 2 == 0) return 0.2;
    return -0.15;
}

/**
 * @brief The corpus's distribution of |interval| between successive notes (0..12+), per role.
 *
 * Built once for all roles inside a function-local static (thread-safe initialisation): the probes
 * plan tracks on several threads at once (Probe.h), and a lazily filled per-role cache raced between
 * them -- one thread read a half-filled histogram, the critic chose another cell, and the plan came
 * out different from the serial one (testProbeSchedule, measured on the first full run of this round).
 */
const std::vector<double>& corpusIntervalHistogram(CorpusRoleId role)
{
    struct Table {
        std::vector<double> h[kNumCorpusRoles];
        Table()
        {
            for (int k = 0; k < kNumCorpusRoles; ++k) {
                std::vector<double>& v = h[k];
                v.assign(13, 0.0);
                const CorpusRole& r = kCorpusRoles[k];
                double total = 0.0;
                for (int i = 0; i < r.numBi; ++i) {
                    const int key = static_cast<int>(r.bi[i].key);
                    const int b = key / kCorpusAlphabet, c = key % kCorpusAlphabet;
                    v[static_cast<size_t>(std::min(12, std::abs(c - b)))] += r.bi[i].count;
                    total += r.bi[i].count;
                }
                if (total > 0.0) for (double& x : v) x /= total;
            }
        }
    };
    static const Table table;
    return table.h[std::clamp(static_cast<int>(role), 0, kNumCorpusRoles - 1)];
}

/** @brief The onset mask of the first bar of a family figure (leadRhythm), the fallback cell. */
uint16_t familyCellMask(Rng& r, const CorpusRole& role, int family)
{
    const std::vector<int> steps = leadRhythm(r, role, family);
    unsigned m = 0;
    for (int s : steps) if (s < 16) m |= 1u << s;
    return static_cast<uint16_t>(m);
}

/**
 * @brief Whether a one-bar mask may be a lead cell: the genre rules that stand above the corpus.
 *
 * At least eight onsets (rule 16), inside the density band, no hole of six sixteenths (cyclic:
 * the cell repeats), the downbeat sounding, and one held note -- a step followed by a rest -- so
 * that the fifth has somewhere to rest (rule 18).
 */
bool cellLegal(unsigned mask)
{
    int on = 0;
    for (int s = 0; s < 16; ++s) on += (mask >> s) & 1u;
    if (on < 8 || on > 15 || (mask & 1u) == 0) return false;
    int longest = 0, cur = 0;
    for (int s = 0; s < 32; ++s) { cur = ((mask >> (s % 16)) & 1u) ? 0 : cur + 1; longest = std::max(longest, cur); }
    if (longest >= 6) return false;
    bool held = false;
    for (int s = 0; s < 16; ++s) held = held || (((mask >> s) & 1u) && !((mask >> ((s + 1) % 16)) & 1u));
    return held;
}

/** @brief cellLegal, and inside the density band (8..10, 10..12 or 12..15 onsets). */
bool cellAdmits(unsigned mask, int band)
{
    static const int kLo[3] = { 8, 10, 12 }, kHi[3] = { 10, 12, 15 };
    int on = 0;
    for (int s = 0; s < 16; ++s) on += (mask >> s) & 1u;
    return on >= kLo[band] && on <= kHi[band] && cellLegal(mask);
}

/**
 * @brief How well a lead mask interlocks with the bass: onsets in the bass's off-beat holes count
 *        for it, onsets on the bass's own off-beat transients against it -- as *shares*, so that
 *        under a rolling bass (every off-beat a transient) every mask is treated alike.
 */
double interlockFactor(unsigned mask, unsigned bassMask, float interlock)
{
    if (bassMask == 0 || interlock <= 0.0f) return 1.0;
    int off = 0, holes = 0, hits = 0;
    for (int s = 0; s < 16; ++s) {
        if (s % 4 == 0 || ((mask >> s) & 1u) == 0) continue;
        ++off;
        if ((bassMask >> s) & 1u) ++hits; else ++holes;
    }
    if (off == 0) return 1.0;
    return std::exp(interlock * (0.6 * holes - 0.4 * hits) / off);
}

/**
 * @brief The cell's rhythm: a corpus template the rules admit, or one of its single-step variants,
 *        weighted by the square root of the template's count and by the interlock with the bass; the
 *        family generator where no template fits the band.
 *
 * Counted on the 64 lead templates: the rules admit 12 masks in the sparse band, 5 in the medium and
 * 2 in the dense one -- the dense corpus bars are the sixteen-onset roll, which has no held note
 * (rule 18) and is the arp's figure, not the lead's. Two masks are no pool, so every admitted template
 * also offers its *variants*: one off-beat sixteenth toggled, kept where the result is legal and in
 * the band, at a third of the template's weight. That is corpus-shaped rhythm with the variety a set
 * needs; and the root of the count rather than the count itself, because weighted by count three of
 * the first four seeds looked at shared one cell.
 */
uint16_t drawCellMask(Rng& r, const CorpusRole& role, int band, unsigned bassMask, float interlock, bool& fromCorpus)
{
    const int family = r.below(3);   // drawn whatever happens below: the fallback and the draws after keep their place
    const CorpusBarMask* bars = kCorpusRoleBars[static_cast<int>(CorpusRoleId::Lead)];
    const int n = kNumCorpusRoleBars[static_cast<int>(CorpusRoleId::Lead)];
    std::vector<double> w;
    std::vector<unsigned> cand;
    auto offer = [&](unsigned mask, double weight) {
        for (size_t k = 0; k < cand.size(); ++k) if (cand[k] == mask) { w[k] = std::max(w[k], weight); return; }
        cand.push_back(mask);
        w.push_back(weight * interlockFactor(mask, bassMask, interlock));
    };
    for (int i = 0; i < n; ++i) {
        if (!cellAdmits(bars[i].mask, band)) continue;
        offer(bars[i].mask, std::sqrt(static_cast<double>(bars[i].count)));
    }
    const size_t templates = cand.size();
    for (size_t k = 0; k < templates; ++k) {
        const unsigned base = cand[k];
        const double weight = w[k] / std::max(1e-9, interlockFactor(base, bassMask, interlock)) / 3.0;
        for (int st = 1; st < 16; ++st) {
            if (st % 4 == 0) continue;   // the beats stay as the template has them
            const unsigned variant = base ^ (1u << st);
            if (cellAdmits(variant, band)) offer(variant, weight);
        }
    }
    const double u = r.uniform();
    fromCorpus = !cand.empty();
    if (!fromCorpus) {
        // The family figure, tried up to eight times for a bar the rules admit; the last one stands.
        uint16_t m = 0;
        for (int t = 0; t < 8; ++t) { m = familyCellMask(r, role, family); if (cellAdmits(m, band)) break; }
        return m;
    }
    double total = 0.0;
    for (double v : w) total += v;
    double x = u * total;
    for (size_t i = 0; i < cand.size(); ++i) { if (x < w[i]) return static_cast<uint16_t>(cand[i]); x -= w[i]; }
    return static_cast<uint16_t>(cand.back());
}

/** @brief The four beat anchors of a bar: the pitch at (or, failing an onset, sounding into) each beat. */
void beatAnchors(const std::vector<int>& steps, const std::vector<int>& rels, int anchors[4])
{
    int last = rels.empty() ? 0 : rels[0];
    for (int beat = 0; beat < 4; ++beat) {
        int hit = 1 << 20;
        for (size_t i = 0; i < steps.size(); ++i) {
            const int s = ((steps[i] % 16) + 16) % 16;
            if (s >= beat * 4 && s < beat * 4 + 4 && hit == (1 << 20)) hit = rels[i];
            if (s < beat * 4 + 4) last = rels[i];
        }
        anchors[beat] = hit != (1 << 20) ? hit : last;
        last = anchors[beat];
    }
}

/**
 * @brief The critic: what makes one cell better than another (generate-and-select).
 *
 * Features of the brief, each a plain number: stable degrees on the metrically heavy positions,
 * no direction zigzag, an interval histogram near the corpus's (weighted by the style's proximity),
 * the corpus skeletons as a prior on the four beat anchors, a tonic somewhere in the bar, three to
 * five pitch classes (the acid's rule 6, which holds for a riff), a riff's ambitus, and no pitch
 * three times running. The weights are the ones that made the seeds of docs/PLAN.md sound as the
 * brief describes; they are not fitted.
 */
double criticScore(const std::vector<int>& steps, const std::vector<int>& rels, int rootOffset, const LeadStyle& ls)
{
    const size_t n = rels.size();
    if (n == 0) return -1e9;
    auto pcOf = [&](int rel) { return ((rel + rootOffset) % 12 + 12) % 12; };
    double score = 0.0;
    // Stable degrees where the metre is heavy.
    double stable = 0.0;
    int heavy = 0;
    for (size_t i = 0; i < n; ++i) {
        if (steps[i] % 4 != 0) continue;
        ++heavy;
        const int inst = lerdahlInstability(pcOf(rels[i]));
        stable += inst == 0 ? 1.0 : (inst == 1 ? 0.8 : (inst == 2 ? 0.4 : 0.0));
    }
    if (heavy > 0) score += ls.stability * stable / heavy;
    // Zigzag: the share of direction reversals among the moving intervals.
    int moves = 0, reversals = 0, lastSign = 0;
    for (size_t i = 1; i < n; ++i) {
        const int d = rels[i] - rels[i - 1];
        if (d == 0) continue;
        const int sign = d > 0 ? 1 : -1;
        if (lastSign != 0 && sign != lastSign) ++reversals;
        if (lastSign != 0) ++moves;
        lastSign = sign;
    }
    if (moves > 0) score -= 0.8 * std::max(0.0, static_cast<double>(reversals) / moves - 0.5);
    // The interval histogram against the corpus's.
    {
        const std::vector<double>& h = corpusIntervalHistogram(CorpusRoleId::Lead);
        std::vector<double> mine(13, 0.0);
        for (size_t i = 1; i < n; ++i) mine[static_cast<size_t>(std::min(12, std::abs(rels[i] - rels[i - 1])))] += 1.0 / static_cast<double>(n - 1);
        double l1 = 0.0;
        for (size_t k = 0; k < 13; ++k) l1 += std::abs(mine[k] - h[k]);
        score -= 0.6 * ls.proximity * l1;
    }
    // The corpus skeletons as a prior on the beat anchors.
    {
        int a[4];
        beatAnchors(steps, rels, a);
        const CorpusSkeleton* sk = kCorpusRoleSkeletons[static_cast<int>(CorpusRoleId::Lead)];
        const int nk = kNumCorpusRoleSkeletons[static_cast<int>(CorpusRoleId::Lead)];
        uint32_t best = 0, count = 0;
        for (int k = 0; k < nk; ++k) {
            best = std::max(best, sk[k].count);
            if (sk[k].d[0] == std::clamp(a[1] - a[0], -12, 12) && sk[k].d[1] == std::clamp(a[2] - a[0], -12, 12) && sk[k].d[2] == std::clamp(a[3] - a[0], -12, 12)) count = sk[k].count;
        }
        if (best > 0 && count > 0) score += 0.3 * std::log1p(static_cast<double>(count)) / std::log1p(static_cast<double>(best));
    }
    // A tonic in the bar, and the fifth; three to five pitch classes; a riff's ambitus; no runs.
    bool tonic = false, fifth = false;
    int classes[12] = {};
    int lo = 1 << 20, hi = -(1 << 20), runs = 0;
    for (size_t i = 0; i < n; ++i) {
        const int pc = pcOf(rels[i]);
        tonic = tonic || pc == 0;
        fifth = fifth || pc == 7;
        classes[pc] = 1;
        lo = std::min(lo, rels[i]);
        hi = std::max(hi, rels[i]);
        if (i >= 2 && rels[i] == rels[i - 1] && rels[i - 1] == rels[i - 2]) ++runs;
    }
    int distinct = 0;
    for (int c : classes) distinct += c;
    score += (tonic ? 0.5 : 0.0) + (fifth ? 0.2 : 0.0);
    score += distinct >= 3 && distinct <= 5 ? 0.3 : (distinct < 3 ? -0.3 : 0.0);
    score += hi - lo <= 7 ? 0.2 : (hi - lo > 10 ? -0.4 : 0.0);
    score -= 0.3 * runs;
    return score;
}

/** @brief One bar of the phrase as it is being built. */
struct LeadBar {
    std::vector<int> steps, rels, lens;
    std::vector<int> src;     ///< the cell note each note derives from, -1 for none
};

/** @} */

/**
 * @brief The lead: one cell, one archetype, one operator per bar (22.09.2026, round "Lead").
 *
 * What the user heard until this round -- "eher zufällig" -- was eight bars drawn note by note
 * under unary constraints: recognisable only where the motif happened to be kept. Now a phrase is
 *
 *  1. a **cell**: one bar whose rhythm is a corpus template the rules admit (cellAdmits: eight to
 *     fifteen onsets in the style's density band, no hole of six sixteenths, a held note), weighted by
 *     the interlock with the track's bass pattern (interlockFactor); whose accents, staccato gates and
 *     slide coins are drawn before any pitch exists; and whose pitches are the best of kLeadCandidates
 *     draws from the corpus model under one weight table (criticScore): scale tones only, the
 *     register prior tightened by the style's homing (von Hippel and Huron 2000: the reversal after a
 *     skip is regression to the tessitura, so one scalar does it), and the Lerdahl stability of a
 *     degree pulled by the metric weight of its position -- heavy positions towards 1 and 5, the odd
 *     sixteenths towards the tension degrees. Strong steps stay chord tones of the tonic (rule 18 and
 *     the Bordun), colour tones stay neighbour tones at drawn slots (rule 1);
 *  2. an **archetype** (kLeadArchetypes): the cell's transposition in scale steps, bar by bar, over
 *     the eight bars -- the contour lives here, on the macro level, and nowhere in the cell;
 *  3. an **operator per bar** (CellOp): the odd bars keep the cell, the even bars answer it -- a
 *     cadence onto the tonic, a step up or down, the end inverted, a shift by a sixteenth, a thinning
 *     -- and the eighth bar always cadences. Every bar is then made legal again (strong steps to the
 *     nearest chord tone, no colour tone outside its slot, the window), so no operator can leave the
 *     rules.
 *
 * Styles are the vector `LeadStyle` (Form.h); the knobs compose.lead_density and compose.pitch_entropy
 * override two of its entries. The rhythm, the operators, the archetype and every per-note coin are
 * drawn before the pitches, so a borrowed mode (ModeMaterial) gets the same design with other pitches.
 */
constexpr int kLeadCandidates = 24;   ///< cells drawn per phrase for the critic to choose from

void makeLead(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src,
              const LeadStyle& ls, int densityKnob, int entropyKnob, unsigned bassMask, const SetMotif& motif)
{
    src.mode = std::clamp(scale, 0, kNumScales - 1);
    Rng r;
    r.seed(seed ^ kSaltLead);
    Rng cr;
    cr.seed(seed ^ kSaltColour ^ 2u);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Lead)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Lead);
    const int root = m.root[kLeadI];
    const int rootOffset = ((root - key) % 12 + 12) % 12;
    const int lo = std::max(m.leadWindowLo - root, kCorpusRelMin), hi = std::min(leadWindowHi(m) - root, kCorpusRelMax);
    const double baseCentre = (m.leadWindowLo + 3.5) - root;   // the window's lower half, as kLeadCentre was for C4 .. B4
    m.colour = colour;
    // The knobs over the style: density band and entropy.
    {
        const double x = densityKnob > 0 ? static_cast<double>(densityKnob - 1) : ls.density * 2.0 + (static_cast<double>(r.uniform()) - 0.5);
        m.leadDensityBand = std::clamp(static_cast<int>(std::lround(x)), 0, 2);
        // A track that states or recalls the set's motif takes the motif's band (SetMotif), unless the
        // user holds the density knob -- then the motif plays only where the knob's band admits it. The
        // draw above is made whatever happens, so the draws after it keep their place.
        if (motif.phrase >= 0 && motif.cell != 0 && densityKnob == 0) m.leadDensityBand = std::clamp<int>(motif.band, 0, 2);
    }
    static const double kEntropy[4] = { 1.0, 0.7, 1.0, 1.4 };
    const double temp = temperature * (entropyKnob > 0 ? kEntropy[std::clamp(entropyKnob, 1, 3)] : ls.entropy);
    auto pcOf = [&](int rel) { return ((rel + rootOffset) % 12 + 12) % 12; };
    const double sigma = kLeadSigma / std::max(0.25f, ls.homing);
    // The register centre the cell is drawn around: the lead's (kLeadCentre). The archetypes are
    // centred on the cell (kLeadArchetypes), so the phrase keeps the cell's register.
    const double centre = baseCentre;
    auto registerW = [&](int rel) { const double z = (rel - centre) / sigma; return std::exp(-0.5 * z * z); };
    double scaleW[12] = {};
    for (int d = 0; d < 7; ++d) {
        const int pc = scaleDegree(scale, d) % 12;
        scaleW[pc] = isColourTone(scale, pc) ? 0.0 : (pc == 0 || pc == 7 ? 1.3 : 1.0);
    }
    int tonicPcs[3];
    chordTones(scale, 0, tonicPcs);   // the tonic chord, whatever the pad plays (the Bordun; round "Harmonik")
    auto isChord = [&](int pc) { return pc == tonicPcs[0] || pc == tonicPcs[1] || pc == tonicPcs[2]; };
    // The weight table: scale tone x register x stability-by-metre, tilted by the measured tension curve.
    auto constraintAt = [&](int step) {
        const double t = tensionAt(CorpusRoleId::Lead, step, 1);
        auto stab = [&](int rel) { return std::exp(-ls.stability * metricWeight(step) * lerdahlInstability(pcOf(rel))); };
        if (step % 8 == 0) {
            const std::vector<uint8_t> s = weightedSet(rootOffset, lo, hi, [&](int rel) {
                const int pc = pcOf(rel);
                return isChord(pc) ? scaleW[pc] * registerW(rel) * stab(rel) : 0.0;
            }, t);
            if (!emptySet(s)) return s;
        }
        return weightedSet(rootOffset, lo, hi, [&](int rel) { return scaleW[pcOf(rel)] * registerW(rel) * stab(rel); }, t);
    };
    auto fits = [&](int step, int rel) {
        return rel >= lo && rel <= hi && constraintAt(step)[static_cast<size_t>(sym(rel))] != 0;
    };
    // The nearest legal pitch to `rel` at `step`: a scale tone, no colour tone, a chord tone on a strong step.
    // `dir` (the bar's shift) breaks ties towards the transposition, so a snapped strong step does not undo it.
    auto legal = [&](int step, int rel, int dir) {
        if (fits(step, rel) && !isColourTone(scale, pcOf(rel))) return rel;
        const int first = dir >= 0 ? 1 : -1;
        for (int d = 1; d <= 12; ++d) {
            for (int c : { rel + first * d, rel - first * d })
                if (c >= lo && c <= hi && inScale(scale, pcOf(c)) && !isColourTone(scale, pcOf(c)) && (step % 8 != 0 || isChord(pcOf(c)))) return c;
        }
        return std::clamp(rel, lo, hi);
    };
    const LineRules rules;
    LineRules cellRules;
    cellRules.loop = true;   // the cell repeats bar after bar: a run across its seam would return in every bar
    const double target = colourTarget(CorpusRoleId::Lead, colour);
    std::vector<int> srcOfW[2];       ///< per phrase: the cell note each note derives from (for the slide coins)
    std::vector<float> slideCoinW[2]; ///< per phrase: the cell notes' slide coins

    for (int w = 0; w < 2; ++w) {
        std::vector<MelodyNote>& ph = m.lead[w];
        ph.clear();
        // ---- 1. the design, before any pitch: the cell's rhythm, the archetype, the operators, the coins
        bool fromCorpus = false;
        uint16_t mask = drawCellMask(r, role, m.leadDensityBand, bassMask, ls.interlock, fromCorpus);
        // The set's motif (SetMotif, 23.09.2026): this phrase plays the set's cell and archetype instead of its
        // own draw -- the first track states it, the recalling track answers it in its own key and mode. The
        // draws are still made, so the coins and pitches after them keep their place; the cell stands only
        // where the band admits it (it always does when the band is the motif's own).
        const bool quote = motif.phrase == w && motif.cell != 0 && cellAdmits(motif.cell, m.leadDensityBand);
        if (quote) { mask = motif.cell; fromCorpus = false; }
        m.leadQuotesSet[w] = quote;
        m.leadCell[w] = mask;
        m.leadCellFromCorpus[w] = fromCorpus;
        m.leadArchetype[w] = drawIndex(r, ls.archetype, kNumLeadArchetypes);
        if (quote) m.leadArchetype[w] = std::clamp<int>(motif.archetype, 0, kNumLeadArchetypes - 1);
        const LeadArchetypeDef& arche = kLeadArchetypes[std::clamp(m.leadArchetype[w], 0, kNumLeadArchetypes - 1)];
        for (int b = 0; b < 8; ++b) {
            int op = static_cast<int>(CellOp::Keep);
            if (b == 7) op = static_cast<int>(CellOp::EndCadence);
            else if (b % 2 == 1) op = drawIndex(r, ls.cellOp, kNumCellOps);
            m.leadOps[w][b] = static_cast<int8_t>(op);
        }
        std::vector<int> cSteps;
        for (int s = 0; s < 16; ++s) if ((mask >> s) & 1u) cSteps.push_back(s);
        const size_t cn = cSteps.size();
        std::vector<int> cLens(cn);
        for (size_t i = 0; i < cn; ++i) cLens[i] = (i + 1 < cn ? cSteps[i + 1] : 16) - cSteps[i];
        std::vector<uint8_t> cFlags(cn, 0);
        std::vector<float> slideCoin(cn);
        for (size_t i = 0; i < cn; ++i) {
            const int s = cSteps[i];
            const bool afterRest = s > 0 && ((mask >> (s - 1)) & 1u) == 0;
            if (s % 4 != 0 && afterRest && r.uniform() < ls.accentChance) cFlags[i] |= kNoteAccent;
            if (cLens[i] == 1 && s % 2 == 1 && r.uniform() < 0.2f) cFlags[i] |= kNoteShort;
            slideCoin[i] = r.uniform();
        }
        const int thinCount = 2 + r.below(2);
        // ---- 2. the cell's pitches: the best of kLeadCandidates draws
        Allowed al;
        for (int s : cSteps) al.push_back(constraintAt(s));
        std::vector<int> cell;
        double bestScore = -1e18;
        for (int c = 0; c < kLeadCandidates; ++c) {
            const std::vector<int> cand = drawPitches(model, CorpusRoleId::Lead, src, al, cSteps, 1, 0, 0, temp, r);
            const double sc = criticScore(cSteps, cand, rootOffset, ls);
            if (sc > bestScore) { bestScore = sc; cell = cand; }
        }
        std::vector<uint8_t> cFixed(cn, 0);
        // Rule 18: the fifth rests on the cell's longest note (the latest of the longest).
        {
            size_t best = cn;
            int bestLen = 1;
            for (size_t k = 0; k < cn; ++k) if (cLens[k] >= bestLen && cLens[k] >= 2) { bestLen = cLens[k]; best = k; }
            int fifth = 1 << 20;
            for (int rel = lo; rel <= hi; ++rel)
                if (pcOf(rel) == 7 && std::fabs(rel - centre) < std::fabs(fifth - centre)) fifth = rel;
            if (best < cn && fifth != (1 << 20) && fits(cSteps[best], fifth)) { cell[best] = fifth; cFixed[best] = 1; }
        }
        enforceLine(model, CorpusRoleId::Lead, src, al, cSteps, 1, cFixed, cellRules, 0, 0, temp, r, cell);
        // Rule 1: the colour slots of the cell, the same slots in every bar that keeps those notes.
        std::vector<int> slots;
        {
            const std::vector<int> cand = colourSlots(cSteps, cLens, cFixed, true, 16, target, cr, [&](int nx) {
                int t = 1 << 20;
                for (int rel = lo; rel <= hi; ++rel) if (pcOf(rel) == 0 && std::abs(rel - cell[static_cast<size_t>(nx)]) < std::abs(t - cell[static_cast<size_t>(nx)])) t = rel;
                return t != (1 << 20) && fits(cSteps[static_cast<size_t>(nx)], t);
            });
            for (int k : cand) if (static_cast<size_t>(k) + 1 < cn) slots.push_back(k);   // a slot's resolution is inside the bar
        }
        // ---- 3. the eight bars: archetype, operator, legality, colour, slides
        std::vector<int> steps, rels, lens, srcOf;
        std::vector<uint8_t> flags, fixed;
        for (int b = 0; b < 8; ++b) {
            const CellOp op = static_cast<CellOp>(m.leadOps[w][b]);
            LeadBar bar;
            for (size_t i = 0; i < cn; ++i) { bar.steps.push_back(cSteps[i]); bar.rels.push_back(cell[i]); bar.src.push_back(static_cast<int>(i)); }
            std::vector<uint8_t> bFlags(cFlags);
            // Rhythm operators.
            if (op == CellOp::Shift16) {
                for (int& s : bar.steps) s = (s + 1) % 16;
                std::vector<size_t> order(cn);
                for (size_t i = 0; i < cn; ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](size_t a, size_t c) { return bar.steps[a] < bar.steps[c]; });
                LeadBar t;
                std::vector<uint8_t> f;
                for (size_t i : order) { t.steps.push_back(bar.steps[i]); t.rels.push_back(bar.rels[i]); t.src.push_back(bar.src[i]); f.push_back(bFlags[i]); }
                bar = t;
                bFlags = f;
            } else if (op == CellOp::Thin) {
                // The weakest notes go: odd sixteenths, one long, not the held fifth and not a slot.
                int dropped = 0;
                for (size_t i = cn; i-- > 0 && dropped < thinCount;) {
                    const bool slot = std::find(slots.begin(), slots.end(), static_cast<int>(i)) != slots.end() || (i > 0 && std::find(slots.begin(), slots.end(), static_cast<int>(i) - 1) != slots.end());
                    if (cSteps[i] % 2 == 0 || cLens[i] != 1 || cFixed[i] || slot || i == 0) continue;
                    unsigned left = 0;
                    for (size_t k = 0; k < bar.steps.size(); ++k) left |= 1u << bar.steps[k];
                    left &= ~(1u << cSteps[i]);
                    if (!cellLegal(left)) continue;   // at least eight onsets, no hole, the held note stay
                    const size_t at = static_cast<size_t>(std::find(bar.src.begin(), bar.src.end(), static_cast<int>(i)) - bar.src.begin());
                    bar.steps.erase(bar.steps.begin() + static_cast<long>(at));
                    bar.rels.erase(bar.rels.begin() + static_cast<long>(at));
                    bar.src.erase(bar.src.begin() + static_cast<long>(at));
                    bFlags.erase(bFlags.begin() + static_cast<long>(at));
                    ++dropped;
                }
                if (dropped == 0) m.leadOps[w][b] = static_cast<int8_t>(CellOp::Keep);   // nothing could go: the report says so
            }
            const size_t bn = bar.steps.size();
            bar.lens.assign(bn, 1);
            for (size_t i = 0; i < bn; ++i) bar.lens[i] = (i + 1 < bn ? bar.steps[i + 1] : 16) - bar.steps[i];
            // The transposition: the archetype's offset plus the operator's, reduced until the window holds it.
            int shift = arche.step[b] + (op == CellOp::TransposeUp ? 1 : (op == CellOp::TransposeDown ? -1 : 0));
            for (;;) {
                bool ok = true;
                for (int rel : bar.rels) { const int t = transposeRel(scale, rootOffset, rel, shift); ok = ok && t >= lo && t <= hi; }
                if (ok || shift == 0) break;
                shift += shift > 0 ? -1 : 1;
            }
            m.leadShift[w][b] = static_cast<int8_t>(shift);
            for (int& rel : bar.rels) rel = transposeRel(scale, rootOffset, rel, shift);
            // Pitch operators on the last beat.
            if (bn >= 2 && (op == CellOp::EndCadence || op == CellOp::InvertEnd)) {
                size_t first = bn;
                for (size_t i = 0; i < bn; ++i) if (bar.steps[i] >= 12 && first == bn) first = i;
                if (first == bn) first = bn - 1;
                if (op == CellOp::EndCadence) {
                    int tonic = 1 << 20;
                    for (int rel = lo; rel <= hi; ++rel) if (pcOf(rel) == 0 && std::abs(rel - bar.rels[bn - 1]) < std::abs(tonic - bar.rels[bn - 1])) tonic = rel;
                    if (tonic != (1 << 20)) {
                        bar.rels[bn - 1] = tonic;
                        if (bn >= 3 && first < bn - 1) {
                            // The approach note: a scale step off the tonic, on the side the note before it comes
                            // from -- but never a colour tone (in Phrygian the step above is the b2, which legal()
                            // would snap straight back onto the tonic: three tonics in a row, measured), so the
                            // other side or a farther step where the nearer ones are colour; in Double Harmonic
                            // b2, 3 and 7 all are, and a tonic at the window's floor left nothing, so the search
                            // goes out to three steps and, failing that, takes the nearest legal tone above.
                            const int side = bar.rels[bn - 3] >= tonic ? 1 : -1;
                            int approach = tonic;
                            for (int cand : { side, -side, 2 * side, -2 * side, 3 * side, -3 * side }) {
                                const int c = transposeRel(scale, rootOffset, tonic, cand);
                                if (c >= lo && c <= hi && !isColourTone(scale, pcOf(c))) { approach = c; break; }
                            }
                            if (approach == tonic) {
                                for (int d = 1; d <= 12 && approach == tonic; ++d)
                                    for (int c : { tonic + d, tonic - d })
                                        if (approach == tonic && c >= lo && c <= hi && inScale(scale, pcOf(c)) && !isColourTone(scale, pcOf(c))) approach = c;
                            }
                            if (approach != tonic) bar.rels[bn - 2] = approach;
                        }
                    }
                } else {
                    const int anchor = bar.rels[first];
                    for (size_t i = first + 1; i < bn; ++i) bar.rels[i] = 2 * anchor - bar.rels[i];
                }
            }
            // Legality: every note a scale tone, no colour tone yet, chord tones on the strong steps, inside the window.
            for (size_t i = 0; i < bn; ++i) bar.rels[i] = legal(bar.steps[i], bar.rels[i], shift);
            // The colour slots: the cell's, wherever the bar kept both the slot's note and its resolution side by side.
            std::vector<uint8_t> bFixed(bn, 0);
            for (size_t i = 0; i < bn; ++i) if (bar.src[i] >= 0 && cFixed[static_cast<size_t>(bar.src[i])] && shift == 0) bFixed[i] = 1;
            for (int k : slots) {
                size_t i = bn;
                for (size_t j = 0; j + 1 < bn; ++j) if (bar.src[j] == k && bar.src[j + 1] == k + 1 && bar.steps[j + 1] == bar.steps[j] + 1 && bar.steps[j] % 2 == 1) i = j;
                if (i == bn || bFixed[i] || bFixed[i + 1]) continue;
                int c = 0, t = 0;
                if (!placeColour(scale, rootOffset, lo, hi, hi, bar.rels[i], cr, c, t)) continue;
                if (!fits(bar.steps[i + 1], t)) continue;
                bar.rels[i] = c;
                bar.rels[i + 1] = t;
                bFixed[i] = bFixed[i + 1] = 1;
            }
            for (size_t i = 0; i < bn; ++i) {
                steps.push_back(bar.steps[i] + 16 * b);
                rels.push_back(bar.rels[i]);
                lens.push_back(bar.lens[i]);
                srcOf.push_back(bar.src[i]);
                flags.push_back(bFlags[i]);
                fixed.push_back(bFixed[i]);
            }
        }
        // ---- 4. no pitch three times in a row, anywhere in the phrase (the cadences and the colour stay)
        {
            const size_t n = steps.size();
            Allowed all;
            for (size_t i = 0; i < n; ++i) all.push_back(constraintAt(steps[i]));
            for (size_t i = n - std::min<size_t>(n, 2); i < n; ++i) fixed[i] = 1;   // the phrase's cadence: its last two notes
            enforceLine(model, CorpusRoleId::Lead, src, all, steps, 8, fixed, rules, 0, 0, temp, r, rels);
        }
        srcOfW[w] = srcOf;
        slideCoinW[w] = slideCoin;
        for (size_t i = 0; i < steps.size(); ++i) {
            MelodyNote note;
            note.step = static_cast<int16_t>(steps[i]);
            note.len = static_cast<int16_t>(lens[i]);
            note.rel = static_cast<int8_t>(rels[i]);
            note.flags = flags[i];
            note.velocity = static_cast<uint8_t>((flags[i] & kNoteAccent) ? 112 : (steps[i] % 8 == 0 ? 104 : 92));
            ph.push_back(note);
        }
    }
    // Across the phrase boundaries: the lead plays phrase 0, 1, 0, 1, ..., so the end of each runs on
    // into the start of the other. Where that makes one pitch sound three times in a row, a note of
    // the *following* phrase's opening moves (22.09.2026: the cadence on the tonic stays -- it is the
    // arrival; before, the last note moved and 93 of 128 cadences landed off the tonic, measured):
    // its second note where the phrase opens with two of the cadence's pitch, else its first.
    for (int w = 0; w < 2; ++w) {
        const std::vector<MelodyNote>& a = m.lead[w];
        std::vector<MelodyNote>& b = m.lead[1 - w];
        const size_t n = a.size();
        if (n < 2 || b.size() < 2) continue;
        const int x1 = a[n - 2].rel, x2 = a[n - 1].rel, y1 = b[0].rel, y2 = b[1].rel;
        const bool two = x2 == y1 && y1 == y2, one = x1 == x2 && x2 == y1;
        if (!two && !one) continue;
        MelodyNote& move = two ? b[1] : b[0];
        const int step = move.step, from = move.rel;
        const int after = b.size() > (two ? 2u : 1u) ? b[two ? 2 : 1].rel : (1 << 20);   // never onto the note after it: no new run
        int best = from, bestD = 1 << 20;
        for (int rel = lo; rel <= hi; ++rel) {
            if (!inScale(scale, pcOf(rel)) || isColourTone(scale, pcOf(rel)) || rel == from || rel == after) continue;
            if (step % 8 == 0 && !isChord(pcOf(rel))) continue;
            if (std::abs(rel - from) < bestD) { bestD = std::abs(rel - from); best = rel; }
        }
        move.rel = static_cast<int8_t>(best);
    }
    // ---- 5. slides, on the final pitches: a tension degree stepping onto a stable one, adjacent
    // notes no more than a whole tone apart, by the cell note's coin (so the modes agree).
    for (int w = 0; w < 2; ++w) {
        std::vector<MelodyNote>& ph = m.lead[w];
        for (size_t i = 0; i + 1 < ph.size(); ++i) {
            const int srcNote = i < srcOfW[w].size() ? srcOfW[w][i] : -1;
            if (srcNote < 0 || ph[i].len != 1 || ph[i + 1].step != ph[i].step + 1) continue;
            const int a = lerdahlInstability(pcOf(ph[i].rel)), z = lerdahlInstability(pcOf(ph[i + 1].rel));
            if (a >= 2 && z <= 1 && std::abs(ph[i + 1].rel - ph[i].rel) <= 2 && slideCoinW[w][static_cast<size_t>(srcNote)] < ls.slideChance)
                ph[i].flags = static_cast<uint8_t>(ph[i].flags | kNoteSlide);
        }
    }
}

/**
 * @brief The arp: continuous sixteenths split into a low anchor stream and a high stream (rules 11-14).
 *
 * Every sixteenth sounds (the corpus's arps play 16 of 16). Which of them belong to the high stream
 * is what the style decides: up, down and up-down alternate low and high on every other sixteenth;
 * a Euclidean arp puts its E(k,16) pulses up and keeps the rest on the anchor, so E(5,16) is five high
 * notes in a continuous line, not five notes and eleven holes; the polymeter's three-sixteenth cell
 * is one high note and two anchors, and the high note precesses against the bar; the corpus style
 * takes a Euclidean mask of five to eight pulses and lets the corpus arp model choose the high notes.
 * Bregman (*Auditory Scene Analysis*, MIT Press 1990) is why the octave split works: a fast
 * alternation between registers an octave apart is heard as two streams, a pedal and a figure.
 *
 * The low stream is the chord root between G3 and F#4 (the tonic where the chord root is itself a
 * colour tone -- the bII of the Phrygian pendulum then sounds over a tonic pedal rather than putting
 * the b2 on every other sixteenth); the high stream is sus2, sus4 or add9 material an octave up
 * (arpHighTones), never above G5. A colour tone appears only as a glint: one weak sixteenth, over the
 * tonic chord, straight back to the anchor. A' moves the last beat's high notes one tone up, A''
 * also takes the bar's last high note to the top; the second set reverses the high stream.
 */
void makeArp(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src)
{
    src.mode = std::clamp(scale, 0, kNumScales - 1);
    Rng r;
    r.seed(seed ^ kSaltArp);
    Rng cr;
    cr.seed(seed ^ kSaltColour ^ 3u);
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Arp);
    const int root = m.root[kArpI];
    const int rootOffset = ((root - key) % 12 + 12) % 12;
    // 16.09.2026: two more step families. Euclidean selection (Toussaint, "The Euclidean algorithm
    // generates traditional musical rhythms", BRIDGES 2005) reuses the percussion's own generator,
    // and the polymeter is a three-sixteenth cell that precesses against the 4/4 bar.
    static const double kArpStyleWeights[kNumArpStyles] = { 0.30, 0.12, 0.10, 0.12, 0.24, 0.12 };
    m.arpStyle = drawIndex(r, kArpStyleWeights, kNumArpStyles);
    m.arpPolymeter = m.arpStyle == static_cast<int>(ArpStyle::Polymeter);
    m.arpOctaveJump = r.uniform() < 0.3f;
    m.arpTones = r.below(3);
    // The rotation at a moderate syncopation, exactly as a Euclidean percussion lane chooses its own
    // (Rhythm.cpp): Sioros, Miron, Davies, Gouyon and Madison ("Syncopation creates the sensation of
    // groove in synthesized music examples", Frontiers in Psychology 2014) found groove rising with
    // moderate syncopation, so neither extreme is taken. Ties go to the lowest rotation, so the choice
    // is a function of the pulses alone and cannot drift.
    auto euclidMask = [&](int pulses, int& rotation) {
        auto syncOf = [&](int rot) {
            const std::vector<bool> e = euclid(pulses, kStepsPerBar, rot);
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
        rotation = best;
        const std::vector<bool> e = euclid(pulses, kStepsPerBar, best);
        uint16_t mask = 0;
        for (int s = 0; s < kStepsPerBar; ++s) if (e[static_cast<size_t>(s)]) mask = static_cast<uint16_t>(mask | (1u << s));
        return mask;
    };
    uint16_t high = 0xAAAA;   // up, down, up-down: every other sixteenth up
    if (m.arpStyle == static_cast<int>(ArpStyle::Euclid)) {
        // E(5,16) and E(7,16) are the two the plan names; their neighbours E(4,16), E(6,16) and
        // E(8,16) are the rest of the family and carry a third of the weight between them.
        static const double kPulseWeights[5] = { 0.10, 0.30, 0.15, 0.30, 0.15 };
        m.arpPulses = 4 + drawIndex(r, kPulseWeights, 5);
        high = euclidMask(m.arpPulses, m.arpRotation);
    } else if (m.arpStyle == static_cast<int>(ArpStyle::Corpus)) {
        static const double kPulseWeights[4] = { 0.25, 0.20, 0.30, 0.25 };
        const int pulses = 5 + drawIndex(r, kPulseWeights, 4);
        int rot = 0;
        high = pulses == 8 ? static_cast<uint16_t>(0xAAAA) : euclidMask(pulses, rot);
    } else if (m.arpPolymeter) {
        high = 0;   // the three-sixteenth cell carries its own: one high note, two anchors
    }
    m.arpHigh = high;
    const int lo = kArpLowest - root, hi = kArpHighest - root;
    const double target = colourTarget(CorpusRoleId::Arp, colour);
    for (int c = 0; c < 4; ++c) {
        int anchor = 0;
        const std::vector<int> tones = arpHighTones(scale, m.chordDegree[c], key, m.arpTones, anchor);
        const int t = static_cast<int>(tones.size());
        const int aRel = anchor - root;
        const bool tonicChord = ((anchor - key) % 12 + 12) % 12 == 0;
        const int cellSteps = m.arpPolymeter ? 3 : kStepsPerBar;
        std::vector<int> steps;
        for (int s = 0; s < cellSteps; ++s) steps.push_back(s);
        auto isHigh = [&](int s) { return m.arpPolymeter ? s == 0 : ((high >> s) & 1u) != 0; };
        // The glints: weak sixteenths straight before a low step, over the tonic chord only.
        std::vector<int> glintAt;
        int glintRel = 0;
        if (tonicChord && !m.arpPolymeter) {
            int res = 0;
            if (placeColour(scale, rootOffset, lo, hi, hi, aRel, cr, glintRel, res) && res == aRel) {
                const std::vector<int> ones(static_cast<size_t>(cellSteps), 1);
                const std::vector<uint8_t> taken(static_cast<size_t>(cellSteps), 0);
                glintAt = colourSlots(steps, ones, taken, true, cellSteps, target, cr, [&](int i) { return !isHigh(i); });
            }
        }
        for (int set = 0; set < kMaterialSets; ++set) {
            // The high stream's order: up, down or up-down through the tones (the second set reverses it).
            const bool reverse = set == 1;
            auto seqAt = [&](int k) {
                int idx;
                if (m.arpStyle == static_cast<int>(ArpStyle::UpDown) || m.arpPolymeter) {
                    const int period = std::max(1, 2 * t - 2);
                    const int ph = (k + (reverse ? t - 1 : 0)) % period;
                    idx = ph < t ? ph : period - ph;
                } else {
                    const bool down = (m.arpStyle == static_cast<int>(ArpStyle::Down)) != reverse;
                    idx = down ? t - 1 - k % t : k % t;
                }
                return tones[static_cast<size_t>(std::clamp(idx, 0, t - 1))];
            };
            std::vector<int> pitch(static_cast<size_t>(cellSteps));
            if (m.arpStyle == static_cast<int>(ArpStyle::Corpus)) {
                // The corpus arp model chooses the high notes; the anchor and the glints are fixed.
                Allowed al;
                for (int s = 0; s < cellSteps; ++s) {
                    if (std::find(glintAt.begin(), glintAt.end(), s) != glintAt.end()) al.push_back(single(glintRel));
                    else if (!isHigh(s)) al.push_back(single(aRel));
                    else al.push_back(weightedSet(rootOffset, lo, hi, [&](int rel) {
                        return std::find(tones.begin(), tones.end(), root + rel) != tones.end() ? 1.0 : 0.0;
                    }, 0.0));
                }
                const std::vector<int> rels = drawPitches(model, CorpusRoleId::Arp, src, al, steps, 1, aRel, aRel, temperature, r);
                for (int s = 0; s < cellSteps; ++s) pitch[static_cast<size_t>(s)] = root + rels[static_cast<size_t>(s)];
                if (reverse) {
                    // The second set: the same high notes in the opposite order (the glints stay put).
                    auto moves = [&](int s) { return isHigh(s) && std::find(glintAt.begin(), glintAt.end(), s) == glintAt.end(); };
                    std::vector<int> hs;
                    for (int s = 0; s < cellSteps; ++s) if (moves(s)) hs.push_back(pitch[static_cast<size_t>(s)]);
                    std::reverse(hs.begin(), hs.end());
                    size_t k = 0;
                    for (int s = 0; s < cellSteps; ++s) if (moves(s)) pitch[static_cast<size_t>(s)] = hs[k++];
                }
            } else {
                int k = 0;
                for (int s = 0; s < cellSteps; ++s) pitch[static_cast<size_t>(s)] = isHigh(s) ? seqAt(k++) : anchor;
                for (int s : glintAt) pitch[static_cast<size_t>(s)] = root + glintRel;
            }
            for (int v = 0; v < kAcidVariants; ++v) {
                std::vector<int> p = pitch;
                if (v >= 1) {
                    // A': the last beat's high notes one tone up (the top wraps to the bottom).
                    for (int s = std::max(0, cellSteps - 4); s < cellSteps; ++s) {
                        if (!isHigh(s) || std::find(glintAt.begin(), glintAt.end(), s) != glintAt.end()) continue;
                        const auto it = std::find(tones.begin(), tones.end(), p[static_cast<size_t>(s)]);
                        const int idx = it == tones.end() ? 0 : static_cast<int>(it - tones.begin());
                        p[static_cast<size_t>(s)] = tones[static_cast<size_t>((idx + 1) % t)];
                    }
                }
                if (v == 2) {
                    // A'': and the bar's last high note to the top (or, already there, to the bottom).
                    for (int s = cellSteps - 1; s >= 0; --s) {
                        if (!isHigh(s) || std::find(glintAt.begin(), glintAt.end(), s) != glintAt.end()) continue;
                        p[static_cast<size_t>(s)] = p[static_cast<size_t>(s)] == tones.back() ? tones.front() : tones.back();
                        break;
                    }
                }
                std::vector<MelodyNote>& cell = m.arp[arpCell(set, v, c)];
                cell.clear();
                for (int s = 0; s < cellSteps; ++s) {
                    MelodyNote n;
                    n.step = static_cast<int16_t>(s);
                    n.len = 1;
                    n.rel = static_cast<int8_t>(p[static_cast<size_t>(s)] - root);
                    n.velocity = s % 4 == 0 ? 104 : (isHigh(s) ? 92 : 80);
                    cell.push_back(n);
                }
            }
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
    // The ranges exist for one thing: the masking rule that keeps the arp out of the lead's register
    // (Form.cpp). Until 18.09.2026 the lead's octave jumps were left out of them; since the lead's
    // register rule (A5 at most, jumps included) every lead note is part of its register.
    for (const auto& ph : m.lead) pitchRange(ph, m.root[kLeadI], m.leadLo, m.leadHi);
    for (const auto& cell : m.arp) pitchRange(cell, m.root[kArpI], m.arpLo, m.arpHi);
    // Every borrowed mode counts towards the ranges as well. The masking rule between lead and arp
    // is decided once per section from these numbers (Form.cpp), and a section may be playing a
    // borrowed mode; taking the union keeps the rule conservative rather than letting a recoloured
    // note slip past it.
    for (const ModeMaterial& mm : m.mode) {
        if (!mm.built) continue;
        for (const auto& ph : mm.lead) pitchRange(ph, m.root[kLeadI], m.leadLo, m.leadHi);
        for (const auto& cell : mm.arp) pitchRange(cell, m.root[kArpI], m.arpLo, m.arpHi);
    }
    // No `arpHi += 12` for the octave jump any more: composeMelodyBar plays a shift only where the whole
    // cell stays inside G3..G5 (rule 14), and the cells already span most of that register.
    if (m.leadLo > m.leadHi) { m.leadLo = m.root[kLeadI]; m.leadHi = m.root[kLeadI]; }
    if (m.arpLo > m.arpHi) { m.arpLo = m.root[kArpI]; m.arpHi = m.root[kArpI]; }
    Rng pr;
    pr.seed(seed ^ kSaltPad);
    static const double kPatternWeights[6] = { 0.35, 0.2, 0.15, 0.1, 0.15, 0.05 };
    m.padGatePattern = drawIndex(pr, kPatternWeights, 6);
    // 22.09.2026, round "Figuren". A second gate pattern, drawn from the same weights but never the
    // same one, so that the sections which do not use the track's own pattern still move differently
    // rather than falling back to no gate at all; and the track's figure.
    do { m.padGateAlt = drawIndex(pr, kPatternWeights, 6); } while (m.padGateAlt == m.padGatePattern);
    // Held keeps the largest share: it is the pad of the genre, and the three articulated figures are
    // a colour a track may take rather than the rule. Weighted, in order:
    // Held, Pulse, Offbeat, Swell, Syncope.
    static const double kFigureWeights[static_cast<int>(PadFigure::Count)] = { 0.34, 0.22, 0.18, 0.13, 0.13 };
    m.padFigure = drawIndex(pr, kFigureWeights, static_cast<int>(PadFigure::Count));
    // A second figure for the grooves, never the drop's: a drop and the groove before it are the two
    // places the pad is articulated at all, and hearing the same figure in both is most of what is
    // left of "immer dasselbe" once the carpet sections are held.
    do {
        m.padFigureGroove = drawIndex(pr, kFigureWeights, static_cast<int>(PadFigure::Count));
    } while (m.padFigureGroove == m.padFigure);
}

const char* const kPadFigureNames[static_cast<int>(PadFigure::Count)] = { "held", "pulse", "offbeat", "swell", "syncope" };

/**
 * @brief The onsets of one chord block for a figure, as (sixteenth from the block's start, beats held).
 *
 * The block is `chordBars` bars long. A figure that names more onsets than fit is cut by the caller;
 * every length is shortened by a sixty-fourth there as well, so a repeated pitch always takes a fresh
 * voice rather than retriggering one that is still sounding.
 * @param figure   PadFigure
 * @param chordBars bars in the block (2 or 4)
 * @param out      receives the onsets
 */
void padOnsets(int figure, int chordBars, std::vector<std::pair<int, double>>& out)
{
    out.clear();
    const int steps = chordBars * kStepsPerBar;
    const double block = chordBars * static_cast<double>(kBeatsPerBar);
    switch (static_cast<PadFigure>(std::clamp(figure, 0, static_cast<int>(PadFigure::Count) - 1))) {
    case PadFigure::Pulse:
        for (int b = 0; b < chordBars; ++b) out.emplace_back(b * kStepsPerBar, static_cast<double>(kBeatsPerBar));
        break;
    case PadFigure::Offbeat:
        // The "and" of two and of four: the two sixteenths the kick and the bass leave free in a
        // four-to-the-floor bar, which is where a stabbed pad sits without fighting either.
        for (int b = 0; b < chordBars; ++b) {
            out.emplace_back(b * kStepsPerBar + 6, 1.5);
            out.emplace_back(b * kStepsPerBar + 14, 1.5);
        }
        break;
    case PadFigure::Swell:
        // Half the block short and quiet, then the whole chord for the rest: the pad arrives at the
        // middle of its own block instead of at its start, which is what makes the next one land.
        out.emplace_back(0, 0.5 * block);
        out.emplace_back(steps / 2, 0.5 * block);
        break;
    case PadFigure::Syncope:
        for (int b = 0; b < chordBars; ++b) {
            out.emplace_back(b * kStepsPerBar, 2.5);
            out.emplace_back(b * kStepsPerBar + 10, 1.5);
        }
        break;
    case PadFigure::Held:
    default:
        out.emplace_back(0, block);
        break;
    }
}

/**
 * @brief The counter-lead: a second lead that answers the first (19.09.2026, round "voices"; the four
 *        modes of 23.09.2026, round "Counter").
 *
 * The user's brief (22.09.2026) and its supplement: a designed dialogue engine instead of a gap filler,
 * with "MOTIF_ECHO" as the cheapest authentic counter, fixed rhythmic answer templates, a complementary
 * contour, and the genre matrix -- Full-On call-and-response, Progressive and Darkpsy timbral, Hi-Tech
 * micro-hocketing. The user's finding of 23.09.2026: "Die Counter-Leads sind bislang kaum hoerbar" --
 * whips of 45 ms at -7 dB, only in drop 2's later groups. Four modes (Form.h, CounterMode), drawn per
 * track from the style's vector or set by compose.counter_mode:
 *
 *  - **Echo**: the lead's own material as its answer -- its beat notes and accents, delayed by a dotted
 *    eighth or a beat, a fifth, an octave or a fourth up, in the contrasting timbre, on steps where the
 *    lead does not strike. Thematically coherent by construction: the brief's MOTIF_ECHO.
 *  - **Answer** (the mode of the rounds before, with templates): after each of the lead's statements A,
 *    A' and A'' the second bar carries an answer in one of four fixed rhythms -- Offbeat Triple Stab
 *    (x.x.x), Fast Cascade (xxxx), Syncopated Hook (x..x..x), Gallop Echo (x..xx..x) -- onto steps where
 *    the lead holds; over B a line of its own. Pitches by the lead's genre rules: the tonic the centre,
 *    chord tones on strong steps, no colour tone, mostly stepwise, the answers resting open and closed
 *    by turns; and a *complementary contour* -- where the lead's bar rose, the answer leans down.
 *  - **Timbral**: not a line but a texture, the Progressive and Forest way: one long note per two-bar
 *    unit (tonic, fifth, third, fifth), a second now and then, a whole bar long, with a sustained
 *    envelope and a portamento the composer writes as offsets on the counter's voice (Composer.cpp,
 *    trackStartControls).
 *  - **Hocket**: the Hi-Tech interlock -- short notes on the off sixteenths where the lead neither
 *    strikes nor holds, stepping up and down from the lead's last pitch, never more than six a bar.
 *
 * The register is the lead's an octave up (MelodyPlan::leadWindowLo + 12), and composeMelodyBar's
 * register guard holds it clear of every lead note sounding at the same instant. Every mode keeps the
 * two rules the score tests read for all of them: no colour tone, and nothing outside the window.
 *
 * **Modal interchange.** The rhythm and every coin are drawn before any pitch, so a borrowed mode gets
 * the same design with other pitches (Melody.h, ModeMaterial).
 */
void makeCounter(MelodyPlan& m, int key, int scale, uint64_t seed)
{
    const int root = m.root[kCounterI];
    const int lo = counterWindowLo(m) - root, hi = counterWindowHi(m) - root;
    // The lead's register centre an octave up, as an interval to this key's counter root (20.09.2026):
    // the counter answers in the lead's register, so it has to be weighted like the lead's.
    const double centre = (m.leadWindowLo + 3.5) + 12.0 - root;
    auto pcOf = [&](int rel) { return (((root - key) + rel) % 12 + 12) % 12; };
    int tonicPcs[3];
    chordTones(scale, 0, tonicPcs);   // the tonic chord: the counter stays home like the lead (22.09.2026)
    auto isChord = [&](int pc) { return pc == tonicPcs[0] || pc == tonicPcs[1] || pc == tonicPcs[2]; };
    // The nearest legal pitch: inside the window, a scale tone, no colour tone, a chord tone where `strong`.
    auto legal = [&](int rel, bool strong) {
        while (rel < lo) rel += 12;
        while (rel > hi) rel -= 12;
        auto ok = [&](int c) { return c >= lo && c <= hi && inScale(scale, pcOf(c)) && !isColourTone(scale, pcOf(c)) && (!strong || isChord(pcOf(c))); };
        if (ok(rel)) return rel;
        for (int d = 1; d <= 12; ++d) { if (ok(rel - d)) return rel - d; if (ok(rel + d)) return rel + d; }
        return std::clamp(rel, lo, hi);
    };
    // The pitch of `pc` nearest to `near` inside the window.
    auto nearestPc = [&](int pc, double near) {
        int best = lo;
        double bestD = 1e9;
        for (int c = lo; c <= hi; ++c)
            if (pcOf(c) == pc && std::fabs(c - near) < bestD) { bestD = std::fabs(c - near); best = c; }
        return best;
    };
    const CounterMode mode = static_cast<CounterMode>(std::clamp(m.counterMode, 0, kNumCounterModes - 1));

    for (int w = 0; w < 2; ++w) {
        Rng r;
        r.seed(mixSeed(seed ^ kSaltCounter, static_cast<uint64_t>(w)));
        const std::vector<MelodyNote>& lead = m.lead[w];
        bool attack[128] = {}, sounding[128] = {};
        int leadRel[128];
        for (int& x : leadRel) x = 1 << 20;
        for (const MelodyNote& n : lead) {
            if (n.step < 0 || n.step >= 128) continue;
            attack[n.step] = true;
            for (int s = n.step; s < std::min(128, n.step + n.len); ++s) { sounding[s] = true; leadRel[s] = n.rel; }
        }
        std::vector<MelodyNote>& out = m.counter[w];
        out.clear();
        auto emitNote = [&](int step, int len, int rel, int velocity) {
            MelodyNote note;
            note.step = static_cast<int16_t>(step);
            note.len = static_cast<int16_t>(std::max(1, len));
            note.rel = static_cast<int8_t>(rel);
            note.velocity = static_cast<uint8_t>(velocity);
            out.push_back(note);
        };

        if (mode == CounterMode::Echo) {
            // The lead's beat notes and accents, delayed and transposed, where the lead does not strike.
            const int delay = r.uniform() < 0.5f ? 3 : 4;
            static const int kIntervals[3] = { 7, 12, 5 };
            const int interval = kIntervals[r.below(3)];
            std::vector<int> st, rl, ln;
            // First on the steps the lead does not strike; where a dense lead leaves fewer than six of them
            // in the phrase, the echo may double the lead's attacks (an octave or a fifth above, in its own
            // timbre -- the doubled echo of the brief's "exakte Kopie, transponiert").
            for (int pass = 0; pass < 2 && st.size() < 6; ++pass) {
                st.clear(); rl.clear(); ln.clear();
                int lastEnd = -1;
                for (const MelodyNote& n : lead) {
                    const bool strong = n.step % 4 == 0 || (n.flags & kNoteAccent) != 0;
                    if (!strong) continue;
                    const int s = n.step + delay;
                    if (s >= 128 || (pass == 0 && attack[s]) || s < lastEnd) continue;
                    // The lead's pitch in the counter's frame: the counter root is the lead root an octave up.
                    const int rel = legal(n.rel + interval - 12, s % 8 == 0);
                    st.push_back(s);
                    rl.push_back(rel);
                    ln.push_back(std::max(1, std::min<int>(n.len, 4)));
                    lastEnd = s + ln.back();
                }
            }
            for (size_t i = 0; i < st.size(); ++i) {
                if (i + 1 < st.size()) ln[i] = std::min(ln[i], st[i + 1] - st[i]);
                emitNote(st[i], ln[i], rl[i], 90);
            }
            continue;
        }
        if (mode == CounterMode::Timbral) {
            // One long note per two-bar unit -- tonic, fifth, third, fifth -- and now and then a second.
            const int cycle[4] = { 0, 7, tonicPcs[1], 7 };
            for (int u = 0; u < 4; ++u) {
                const int rel = nearestPc(cycle[u], centre);
                emitNote(32 * u, 16, rel, 84);
                if (r.uniform() < 0.5f) emitNote(32 * u + 16, 12, nearestPc(u % 2 == 0 ? 7 : 0, rel), 80);
            }
            continue;
        }
        if (mode == CounterMode::Hocket) {
            // The off sixteenths the lead does not strike (it may hold: the hocket sits an octave above),
            // stepping up and down from its last pitch. Measured with "neither strikes nor holds": a dense
            // cell with a held note left no free sixteenth and the hocket fell silent in 5 of 26 drops.
            int last = nearestPc(0, centre), dir = 1, perBar = 0;
            for (int s = 0; s < 128; ++s) {
                if (s % 16 == 0) perBar = 0;
                const float coin = r.uniform();   // drawn for every step: the modes stay in step
                if (attack[s] || s % 2 == 0 || perBar >= 6 || coin >= 0.7f) continue;
                int base = last;
                for (int k = s - 1; k >= 0 && k >= s - 4; --k) if (leadRel[k] != (1 << 20)) { base = leadRel[k]; break; }   // the lead's rel, an octave up in this frame
                const int rel = legal(transposeRel(scale, ((root - key) % 12 + 12) % 12, legal(base, false), dir * (1 + (s / 16) % 2)), false);
                emitNote(s, 1, rel, 96);
                last = rel;
                dir = -dir;
                ++perBar;
            }
            continue;
        }

        // ---- Answer: the mode of the rounds before, with the brief's four rhythmic templates.
        std::vector<int> steps;
        std::vector<int> blockEnd;   // the step each note's answer block ends at
        static const int kTemplates[4][4] = { { 1, 3, 5, -1 }, { 0, 1, 2, 3 }, { 0, 3, 6, -1 }, { 0, 3, 4, 7 } };
        const int tpl = r.below(4);
        // The answers after A, A' and A'': the second bar of each two-bar statement, in the template's
        // rhythm from the bar's second beat, onto steps where the lead holds -- a step the lead strikes is
        // moved on by one, or dropped; a window the lead fills entirely takes one note above its attack.
        for (int bar : { 1, 3, 7 }) {
            const int first = bar * 16 + 4;
            std::vector<int> picked;
            for (int k : kTemplates[tpl]) {
                if (k < 0) continue;
                int s = first + k;
                if (attack[s] && s + 1 < first + 12 && !attack[s + 1]) ++s;
                if (attack[s]) continue;
                if (!picked.empty() && picked.back() == s) continue;
                picked.push_back(s);
            }
            if (picked.empty()) picked.push_back(first);
            for (int s : picked) { steps.push_back(s); blockEnd.push_back(bar * 16 + 16); }
        }
        // The line over B: eighths with two or three sixteenth pickups onto the lead's held steps.
        {
            std::vector<int> b;
            for (int s = 64; s < 96; s += 2) b.push_back(s);
            double wgt[16];
            for (int k = 0; k < 16; ++k) wgt[k] = attack[65 + 2 * k] ? 0.25 : 1.0;
            const int picks = 2 + r.below(2);
            for (int c = 0; c < picks; ++c) {
                const int k = drawIndex(r, wgt, 16);
                b.push_back(65 + 2 * k);
                wgt[k] = 0.0;
            }
            std::sort(b.begin(), b.end());
            for (int s : b) { steps.push_back(s); blockEnd.push_back(96); }
        }
        std::vector<size_t> order(steps.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return steps[a] < steps[b]; });
        std::vector<int> st, be;
        for (size_t i : order) { st.push_back(steps[i]); be.push_back(blockEnd[i]); }
        const size_t n = st.size();
        std::vector<int> lens(n);
        for (size_t i = 0; i < n; ++i) {
            const int next = i + 1 < n ? std::min(st[i + 1], be[i]) : be[i];
            lens[i] = std::max(1, next - st[i]);
        }
        // The lead's contour in the bar before each answer: where it rose, the answer leans down (the
        // brief's complementary contour), and the other way round.
        auto leadDirection = [&](int bar) {
            double first = 0.0, second = 0.0;
            int n1 = 0, n2 = 0;
            for (const MelodyNote& x : lead) {
                if (x.step / 16 != bar) continue;
                if (x.step % 16 < 8) { first += x.rel; ++n1; } else { second += x.rel; ++n2; }
            }
            if (n1 == 0 || n2 == 0) return 0;
            const double d = second / n2 - first / n1;
            return d > 0.5 ? 1 : (d < -0.5 ? -1 : 0);
        };
        // Pitches: one weighted draw per note.
        std::vector<int> rels(n, 0);
        int prev = 0, prev2 = 1000;
        int block = 0;   // which answer this note belongs to (the resting tones alternate by it)
        for (size_t i = 0; i < n; ++i) {
            const int s = st[i];
            const bool strong = s % 8 == 0;
            const bool last = i + 1 == n || be[i + 1] != be[i];   // the answer's last note
            if (i > 0 && be[i] != be[i - 1]) ++block;              // a new answer begins
            const int lean = -leadDirection(s / 16 - 1);          // against the lead's bar before
            std::vector<double> wgt;
            std::vector<int> cand;
            for (int rel = lo; rel <= hi; ++rel) {
                const int pc = pcOf(rel);
                if (!inScale(scale, pc) || isColourTone(scale, pc)) continue;
                if (strong && !isChord(pc)) continue;
                if (rel == prev && prev == prev2) continue;   // never one pitch three times in a row
                // The tonic as the line's centre: 3.0 against 1.4 for the fifth and 1 for the rest (2.0 until
                // 22.09.2026; with the resting tones no longer leaning on the bar's chord the tonic fell to
                // 19 % of the notes, measured, and the fifth sat where the register prior favoured it; the
                // lead's new cells (round "Lead") hold other steps, and 2.5 left the tonic at the bound).
                double v = pc == 0 ? 3.0 : (pc == 7 ? 1.4 : 1.0);
                v *= std::exp(-std::abs(rel - prev) / 2.5);
                if (i > 0 && be[i] == be[i - 1] && lean != 0) v *= std::exp(0.35 * lean * (rel - prev));   // the complementary contour
                const double z = (rel - centre) / 4.0;   // the lead's sigma, on the lead's window an octave up
                v *= std::exp(-0.5 * z * z);
                cand.push_back(rel);
                wgt.push_back(v);
            }
            const double u = r.uniform();   // the one draw of this note, whatever the mode
            int rel = prev;
            if (!cand.empty()) {
                double total = 0.0;
                for (double v : wgt) total += v;
                double x = u * total;
                rel = cand.back();
                for (size_t k = 0; k < cand.size(); ++k) { if (x < wgt[k]) { rel = cand[k]; break; } x -= wgt[k]; }
            }
            if (last) {
                // The resting tone. Until 22.09.2026 "the fifth where the bar's chord takes it, else the
                // tonic" -- but the lines anchor the tonic now (round "Harmonik"; the pendulum is the pad's,
                // the Bordun holds), and against the tonic chord alone every answer rested on the fifth.
                // Instead the answers rest open and closed by turns: the first on the fifth (a question),
                // the second on the tonic (its answer), the line over B open again, and the phrase's very
                // last note the tonic. The octave nearest the drawn note.
                const bool phraseEnd = i + 1 == n;
                const bool fifthFits = !phraseEnd && block % 2 == 0;
                const int want = fifthFits ? 7 : 0;
                int best = rel, bestD = 1 << 20;
                for (int c = lo; c <= hi; ++c)
                    if (pcOf(c) == want && std::abs(c - rel) < bestD && !(c == prev && prev == prev2)) { bestD = std::abs(c - rel); best = c; }
                rel = best;
            }
            rels[i] = rel;
            prev2 = prev;
            prev = rel;
        }
        for (size_t i = 0; i < n; ++i) emitNote(st[i], lens[i], rels[i], st[i] % 4 == 0 ? 100 : 88);
    }
}

/**
 * @brief The stab's chord over one of the track's chords: root position, the arp's material (Melody.h).
 *
 * The root is the arp's anchor (the chord root between G3 and F#4, the tonic where the chord root is
 * itself a colour tone); above it sus2 (1 2 5 8), sus4 (1 4 5 8) or add9 (1 3 5 9) -- "sus2/sus4/add9
 * material like the arp, root position" in the brief's words. A fifth that is not perfect is left out,
 * and no note is a colour tone.
 */
std::vector<int> stabChordImpl(int scale, int degree, int key, int tones)
{
    int d = degree;
    if (isColourTone(scale, scaleDegree(scale, d) % 12)) d = 0;
    const int rootPc = ((key + scaleDegree(scale, d)) % 12 + 12) % 12;
    const int root = kArpLowest + ((rootPc - kArpLowest) % 12 + 12) % 12;   // G3 .. F#4
    auto iv = [&](int k) { return scaleDegree(scale, d + k) - scaleDegree(scale, d); };
    std::vector<int> rel = { 0 };
    if (tones == 0) rel.push_back(iv(1));
    else if (tones == 1) rel.push_back(iv(3));
    else rel.push_back(iv(2));
    if (iv(4) == 7) rel.push_back(7);
    rel.push_back(tones == 2 ? 12 + iv(1) : 12);
    std::vector<int> out;
    for (int x : rel) {
        const int p = root + x;
        if (isColourTone(scale, ((p - key) % 12 + 12) % 12)) continue;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/**
 * @brief The stab's rhythm (19.09.2026): short chord hits on syncopated sixteenths, never on a beat.
 *
 * A Euclidean figure E(k, 16) with k of 3, 4 or 5, turned so that no onset lands on a beat -- the kick
 * is on every beat, and the brief says "never on the kick" (Toussaint 2005 for the family; the
 * rotations that avoid the beats are enumerated rather than searched). The second bar of the two-bar
 * cell drops the first bar's last hit or moves it by an eighth, a small answer. Which bars of a
 * four-bar phrase carry the cell is the track's: the second and the fourth, the fourth alone, the first
 * and the third, or all four -- sparse enough that the stab stays a surprise, which the form thins
 * further by giving it only every other eight-bar group (Form.cpp).
 */
void makeStab(MelodyPlan& m, uint64_t seed)
{
    Rng r;
    r.seed(seed ^ kSaltStab);
    m.stabTones = m.arpTones;   // the arp's sus material, so the two agree
    std::vector<uint16_t> masks;
    const int k = 3 + r.below(3);
    for (int rot = 0; rot < kStepsPerBar; ++rot) {
        const std::vector<bool> e = euclid(k, kStepsPerBar, rot);
        uint16_t mask = 0;
        bool ok = true;
        for (int s = 0; s < kStepsPerBar; ++s) {
            if (!e[static_cast<size_t>(s)]) continue;
            if (s % 4 == 0) { ok = false; break; }
            mask = static_cast<uint16_t>(mask | (1u << s));
        }
        if (ok && mask != 0) masks.push_back(mask);
    }
    if (masks.empty()) masks.push_back(static_cast<uint16_t>((1u << 3) | (1u << 6) | (1u << 11)));   // x..x..x on the off-sixteenths
    const uint16_t a = masks[static_cast<size_t>(r.below(static_cast<int>(masks.size())))];
    uint16_t b = a;
    int lastHit = 15;
    while (lastHit >= 0 && ((a >> lastHit) & 1u) == 0) --lastHit;
    if (r.uniform() < 0.5f && lastHit >= 0) {
        b = static_cast<uint16_t>(a & ~(1u << lastHit));   // the answer drops the last hit
    } else if (lastHit >= 0) {
        const int moved = lastHit >= 2 ? lastHit - 2 : lastHit + 2;   // or moves it by an eighth
        if (moved % 4 != 0 && ((a >> moved) & 1u) == 0) b = static_cast<uint16_t>((a & ~(1u << lastHit)) | (1u << moved));
    }
    m.stabMask[0] = a;
    m.stabMask[1] = b != 0 ? b : a;
    static const uint8_t kBars[4] = { 0xA, 0x8, 0x5, 0xF };
    static const double kBarWeights[4] = { 0.40, 0.30, 0.15, 0.15 };
    m.stabBars = kBars[drawIndex(r, kBarWeights, 4)];
}

/**
 * @brief The tonic drone's own decisions (19.09.2026): whether it adds the octave, and the period of
 *        its slow evolution (8, 16 or 32 bars, the range of the brief). The notes themselves are the
 *        root and the fifth, placed by composeMelodyBar where the form's runs say.
 */
void makeDrone(MelodyPlan& m, uint64_t seed)
{
    Rng r;
    r.seed(seed ^ kSaltDrone);
    m.droneOctave = r.uniform() < 0.5f;
    static const int kPeriods[3] = { 8, 16, 32 };
    m.droneEvolveBars = kPeriods[r.below(3)];
}

} // namespace

std::vector<int> stabChord(int scale, int degree, int key, int tones) { return stabChordImpl(scale, degree, key, tones); }

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

const char* const kLeadArchetypeNames[kNumLeadArchetypes] = { "Phrygian Surge", "Arch & Drop", "Pedal & Bounce", "Descending Cascade", "Tension Call" };
const char* const kCellOpNames[kNumCellOps] = { "keep", "cadence", "up", "down", "invert", "shift", "thin" };

double leadArc(int archetype, int barInPhrase)
{
    const LeadArchetypeDef& a = kLeadArchetypes[std::clamp(archetype, 0, kNumLeadArchetypes - 1)];
    double mean = 0.0;
    for (float v : a.arc) mean += v;
    mean /= 8.0;
    return a.arc[((barInPhrase % 8) + 8) % 8] - mean;
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

uint16_t drawMotifCell(uint64_t seed, int band)
{
    Rng r;
    r.seed(seed);
    bool fromCorpus = false;
    return drawCellMask(r, kCorpusRoles[static_cast<int>(CorpusRoleId::Lead)], std::clamp(band, 0, 2), 0u, 0.0f, fromCorpus);
}

MelodyPlan makeMelodyPlan(const ParamStore& p, const StyleProfile& style, uint64_t seed, int key, int scale,
                          bool firstTrack, float colour, uint32_t scaleMask, unsigned bassMask, const SetMotif& motif)
{
    const int cb = p.base(Module::Compose);
    const double temperature = p.get(cb + compose::MelodyTemperature);
    // 22.09.2026, round "Lead": the two knobs over the style's lead vector (Form.h, LeadStyle); 0 = Auto.
    const int leadDensity = p.getInt(cb + compose::LeadDensity);
    const int pitchEntropy = p.getInt(cb + compose::PitchEntropy);
    const int counterKnob = p.getInt(cb + compose::CounterMode);   // 23.09.2026: 0 = Auto (the style's weights)
    const float mv = p.get(cb + compose::MelodyVariation);
    MelodyPlan m;
    Rng r;
    r.seed(seed);
    // The style profile scales the knobs' amounts; Full-On, the default, scales everything by one.
    static const int kAmountKnob[kMelodyParts] = { compose::AcidAmount, compose::LeadAmount, compose::CounterAmount, compose::ArpAmount,
                                                   compose::StabAmount, compose::PadAmount, compose::DroneAmount };
    float amounts[kMelodyParts];
    for (int k = 0; k < kMelodyParts; ++k) amounts[k] = std::clamp(p.get(cb + kAmountKnob[k]) * style.partAmount[k], 0.0f, 1.0f);
    // The four older parts draw first and in their old order (acid, lead, arp, pad), so that which of
    // them a track has did not move when the three new voices joined (19.09.2026); then the new ones.
    // The counter-lead answers the lead, so a track without a lead has none.
    for (int k = 0; k < kMelodyParts; ++k) m.amount[k] = amounts[k];
    for (int k : { kAcidI, kLeadI, kArpI, kPadI }) m.present[k] = r.uniform() < amounts[k];
    // 21.09.2026: the lead is not a coin flip any more. Drawing it per track meant that "Lead
    // Amount 0.5" gave half the tracks a lead and the other half none at all -- measured over four
    // seeds, two of them had no lead in 256 bars, and with it went the counter-lead, which only
    // answers where the lead plays. A track without its lead is the complaint this fixes. The knob
    // now says how much the lead plays, not whether it exists: the draw above is left standing so
    // that the other parts' draws keep their place in the random stream, and Form.cpp's per-section
    // draw uses `amount` where it used a fixed 0.85.
    // 22.09.2026: acid, arp, pad and drone follow the lead (21.09.2026). The draws above still
    // happen, so nothing else this plan decides moves -- but their answer is no longer used. A coin
    // flip per *track* meant a track simply had no acid line at all (0.6 at the knob's default), no
    // arp (0.5), no drone (0.6), and it is what the user reported on 21.09: "Leads gibt es jetzt gar
    // nicht mehr, ebensowenig wie eine Drone oder Acid-Lines". Measured on seed 7, the first track
    // had none of the three. The knob still says how much each plays; it says it per *section* now
    // (Form.cpp, the section draws), so a line comes and goes inside a track instead of being absent
    // from the whole of it -- which is how a psytrance arrangement works anyway.
    for (int k : { kAcidI, kLeadI, kArpI, kPadI }) m.present[k] = amounts[k] > 0.0f;
    // Every track has at least one melodic part unless all three amounts are zero: the likeliest one.
    if (!m.present[kAcidI] && !m.present[kLeadI] && !m.present[kArpI]) {
        static const int kLine[3] = { kAcidI, kLeadI, kArpI };
        int best = 0;
        for (int k = 1; k < 3; ++k) if (amounts[kLine[k]] * r.uniform() > amounts[kLine[best]] * r.uniform()) best = k;
        m.present[kLine[best]] = amounts[kLine[best]] > 0.0f;
    }
    for (int k : { kCounterI, kStabI, kDroneI }) m.present[k] = r.uniform() < amounts[k];
    m.present[kDroneI] = amounts[kDroneI] > 0.0f;   // see above: its share is the sections', not the tracks'
    m.present[kCounterI] = m.present[kCounterI] && m.present[kLeadI];
    // 20.09.2026, round "climax-polish": the guarantee above can be satisfied by the acid alone, and acid
    // is not a "line" for the presence match or for a drop's brightness (Composer.cpp, matchPresence and
    // probeLoudness's isLine: lead, counter, arp, stab). Measured over 30 tracks: 5 of them had none of
    // those four in their drops -- matchPresence corrects a level, it cannot lift a part that never plays,
    // so those tracks stayed dark no matter the cap. Counter is not a candidate here: it never plays alone
    // (it answers the lead, the line above), so forcing it on would not open a lead-less drop. Lead and
    // arp play in every drop that has them (the instrumentation matrix's `drop || draw`, Form.cpp); the
    // stab only in three quarters of sections there (its own per-section draw) -- so lead and arp are
    // tried first, the same weighted draw as the guarantee above, and the stab only if both their amounts
    // are zero. Scoped to the tracks that actually have nothing: a track that already carries a line does
    // not have its melody touched.
    if (!m.present[kLeadI] && !m.present[kArpI] && !m.present[kStabI]) {
        static const int kVoice[3] = { kLeadI, kArpI, kStabI };
        int best = 0;
        for (int k = 1; k < 3; ++k) if (amounts[kVoice[k]] * r.uniform() > amounts[kVoice[best]] * r.uniform()) best = k;
        m.present[kVoice[best]] = amounts[kVoice[best]] > 0.0f;
    }
    m.key = ((key % 12) + 12) % 12;
    m.root[kAcidI] = kAcidLowest + ((key - 2) % 12 + 12) % 12;   // D3 .. C#4
    // 20.09.2026, round "dialogue": the lead's tonic moved down into the rule's window. With the root at
    // E4 .. D#5 and the window narrowed to C4 .. G4, the highest keys would have had a window of four
    // semitones and the lowest one of eight; from C4 .. B4 every key keeps the full fifth, and every
    // interval to the root still fits the corpus alphabet (kCorpusRelMin .. kCorpusRelMax).
    // 23.09.2026, round "Counter": the window stands the style's registerShift above C4, with a semitone of
    // jitter per track (the literature's 250 Hz .. 2 kHz, weight at 500 Hz .. 1 kHz; the user: "teils zu tief").
    m.leadWindowLo = kLeadLowest + std::clamp(style.lead.registerShift + (r.below(3) - 1), 0, 12);
    m.root[kLeadI] = m.leadWindowLo + ((key - m.leadWindowLo) % 12 + 12) % 12;   // the window's octave
    m.root[kCounterI] = m.root[kLeadI] + 12;                      // C5 .. B5: the lead's register an octave up
    m.root[kArpI] = 57 + ((key - 9) % 12 + 12) % 12;            // A3 .. G#4
    m.root[kStabI] = m.root[kArpI];                               // unused: the stab's chord carries its own root
    m.root[kPadI] = kPadLowest;                                   // unused: the voicings carry their own
    m.root[kDroneI] = kDroneLowest + ((key - 2) % 12 + 12) % 12;  // D2 .. C#3: the drone's low root
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
    makeAcid(m, key, scale, seed, temperature, colour, src);
    makeLead(m, key, scale, seed, temperature, colour, src, style.lead, leadDensity, pitchEntropy, bassMask, motif);
    makeArp(m, key, scale, seed, temperature, colour, src);
    for (int c = 0; c < 4; ++c) m.padVoicing[c] = voiceChord(scale, key, m.chordDegree[c], m.chordType[c], c > 0 ? &m.padVoicing[c - 1] : nullptr);
    for (int c = 0; c < 4; ++c) m.breakVoicing[c] = voiceChord(scale, key, m.breakDegree[c], m.breakType[c], c > 0 ? &m.breakVoicing[c - 1] : nullptr);
    if (m.secondHalf)
        for (int c = 0; c < 4; ++c) m.padVoicing2[c] = voiceChord(scale, key, m.chordDegree2[c], m.chordType2[c], c > 0 ? &m.padVoicing2[c - 1] : nullptr);
    // The three new voices (19.09.2026), each from a salt of its own, after everything older: nothing
    // any older maker drew moves because of them.
    // The counter's mode (23.09.2026): the knob, else the style's weights, from the counter's own stream.
    {
        Rng cm;
        cm.seed(mixSeed(seed ^ kSaltCounter, 77u));
        m.counterMode = counterKnob > 0 ? counterKnob - 1 : drawIndex(cm, style.lead.counterMode, kNumCounterModes);
    }
    makeCounter(m, key, scale, seed);
    makeStab(m, seed);
    makeDrone(m, seed);
    // Modal interchange (Form.h): the material of every mode a section of the form borrows. The
    // makers are run again with the *same* seed and another mode, so the rhythm, the accents and the
    // slides come out identical and only the pitches are recoloured -- interchange rather than a
    // second riff. The scratch plan carries the chords, the roots and the part switches, which are
    // the track's and do not belong to a mode. Nothing here can move the base mode's material: the
    // makers above have already run, and each of these calls starts its own Rng.
    for (int sc = 0; sc < kNumScales; ++sc) {
        if (sc == m.scale || (scaleMask & (1u << sc)) == 0) continue;
        MelodyPlan tmp = m;
        makeAcid(tmp, key, sc, seed, temperature, colour, src);
        makeLead(tmp, key, sc, seed, temperature, colour, src, style.lead, leadDensity, pitchEntropy, bassMask, motif);
        makeArp(tmp, key, sc, seed, temperature, colour, src);
        makeCounter(tmp, key, sc, seed);
        for (int c = 0; c < 4; ++c)
            tmp.padVoicing[c] = voiceChord(sc, key, tmp.chordDegree[c], tmp.chordType[c], c > 0 ? &tmp.padVoicing[c - 1] : nullptr);
        for (int c = 0; c < 4; ++c)
            tmp.breakVoicing[c] = voiceChord(sc, key, tmp.breakDegree[c], tmp.breakType[c], c > 0 ? &tmp.breakVoicing[c - 1] : nullptr);
        if (m.secondHalf)
            for (int c = 0; c < 4; ++c)
                tmp.padVoicing2[c] = voiceChord(sc, key, tmp.chordDegree2[c], tmp.chordType2[c], c > 0 ? &tmp.padVoicing2[c - 1] : nullptr);
        ModeMaterial& mm = m.mode[sc];
        for (int i = 0; i < kAcidCells; ++i) mm.acid[i] = tmp.acid[i];
        for (int i = 0; i < 2; ++i) mm.lead[i] = tmp.lead[i];
        for (int i = 0; i < 2; ++i) mm.counter[i] = tmp.counter[i];
        for (int i = 0; i < kArpCells; ++i) mm.arp[i] = tmp.arp[i];
        for (int c = 0; c < 4; ++c) mm.padVoicing[c] = tmp.padVoicing[c];
        for (int c = 0; c < 4; ++c) mm.breakVoicing[c] = tmp.breakVoicing[c];
        for (int c = 0; c < 4; ++c) mm.padVoicing2[c] = tmp.padVoicing2[c];
        mm.built = true;
    }
    makeRangesAndPad(m, seed);

    Rng s;
    s.seed(seed ^ kSaltSound);
    if (!firstTrack) {
        // The older parts' draws first and in their old order, then the new voices' (19.09.2026).
        for (int k : { kAcidI, kLeadI, kArpI, kPadI }) m.recipe[k] = (2.0f * s.uniform() - 1.0f) * mv;
        const float chance = std::clamp(p.get(cb + compose::SquelchChance) * style.squelchChance, 0.0f, 1.0f);
        m.acidSquelch = s.uniform() < chance ? 1 : 0;
        const float o = s.uniform();
        m.leadOsc = o < 0.65f ? static_cast<int>(PolyOsc::Supersaw) : (o < 0.85f ? static_cast<int>(PolyOsc::Fm) : static_cast<int>(PolyOsc::Va));
        static const int kLeft[3] = { 2, 1, 4 }, kRight[3] = { 3, 2, 1 };
        for (int k : { kAcidI, kLeadI, kArpI }) { m.delay[k][0] = kLeft[s.below(3)]; m.delay[k][1] = kRight[s.below(3)]; }
        for (int k : { kCounterI, kStabI, kDroneI }) m.recipe[k] = (2.0f * s.uniform() - 1.0f) * mv;
    }
    return m;
}

BarPlan allPartsBar(const MelodyPlan& m)
{
    BarPlan bp;
    bp.parts = 0;
    for (int k = 0; k < kMelodyParts; ++k) if (m.present[k]) bp.parts = static_cast<uint8_t>(bp.parts | partBit(static_cast<MelodyPart>(k)));
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

namespace {

/**
 * @brief The pitch span each line voice occupies at each sixteenth of a bar (the register guard).
 *
 * A voice's entry at step s is the lowest and the highest of its notes that sound during s, or empty.
 * A lead note sounds from its onset for as many sixteenths as it is long; a counter note likewise; a
 * stab chord and an arp note sound on their onset sixteenth (their gates are shorter than a sixteenth
 * plus a release of a few tens of milliseconds).
 */
struct RegisterMap {
    int lo[kStepsPerBar];
    int hi[kStepsPerBar];
    RegisterMap() { for (int s = 0; s < kStepsPerBar; ++s) { lo[s] = 1000; hi[s] = -1000; } }
    /** @brief Marks @p pitch as sounding from @p first for @p span sixteenths (clipped to the bar). */
    void add(int first, int span, int pitch)
    {
        for (int s = std::max(0, first); s < std::min(kStepsPerBar, first + std::max(1, span)); ++s) {
            lo[s] = std::min(lo[s], pitch);
            hi[s] = std::max(hi[s], pitch);
        }
    }
    /** @brief Whether the span [a, b] at step @p s keeps kRegisterGap semitones from everything marked. */
    bool clear(int s, int a, int b) const
    {
        if (s < 0 || s >= kStepsPerBar || lo[s] > hi[s]) return true;
        return a - hi[s] >= kRegisterGap || lo[s] - b >= kRegisterGap;
    }
};

/**
 * @brief The octave a line voice takes in a bar where others already sound: the shift among
 *        @p shifts (tried in order, so the first is preferred on a tie) that leaves the fewest of its
 *        events colliding, within [lo, hi].
 * @param events (step, lowest pitch, highest pitch) of each event before any shift
 */
int bestShift(const std::vector<std::array<int, 3>>& events, const RegisterMap& taken, std::initializer_list<int> shifts, int lo, int hi)
{
    int best = 0, bestBad = 1 << 20;
    bool any = false;
    for (int sh : shifts) {
        int bad = 0;
        bool fits = true;
        for (const auto& e : events) {
            if (e[1] + sh < lo || e[2] + sh > hi) { fits = false; break; }
            if (!taken.clear(e[0], e[1] + sh, e[2] + sh)) ++bad;
        }
        if (!fits) continue;
        if (!any || bad < bestBad) { best = sh; bestBad = bad; any = true; }
    }
    return best;
}

} // namespace

void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale,
                      const BarPlan& bp, std::vector<NoteEvent>& out, const MelodyContext& ctx)
{
    (void)scale;
    const ModeMaterial* mat = materialOf(m, bp.scale);
    const std::vector<MelodyNote>* acid = mat != nullptr ? mat->acid : m.acid;
    const std::vector<MelodyNote>* lead = mat != nullptr ? mat->lead : m.lead;
    const std::vector<MelodyNote>* counter = mat != nullptr ? mat->counter : m.counter;
    const std::vector<MelodyNote>* arp = mat != nullptr ? mat->arp : m.arp;
    const std::vector<int>* voicing = mat != nullptr ? mat->padVoicing : m.padVoicing;
    const std::vector<int>* breakVoicing = mat != nullptr ? mat->breakVoicing : m.breakVoicing;
    const std::vector<int>* voicing2 = mat != nullptr ? mat->padVoicing2 : m.padVoicing2;   // the second half's (23.09.2026)
    const int set = std::clamp(ctx.material, 0, kMaterialSets - 1);
    const int cb = p.base(Module::Compose);
    const float swing = p.get(cb + compose::Swing);
    const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
    const uint8_t parts = bp.parts;
    auto has = [&](MelodyPart part) { return (parts & partBit(part)) != 0; };
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

    if (has(MelodyPart::Acid)) {
        // A A A' A'' over four cell lengths (rule 4): four bars for a one-bar cell, eight for two.
        const int steps = m.acidSteps;
        const int cellBars = steps / 16;
        const int inPhrase = barInTrack % (4 * cellBars);
        const int offset = (inPhrase % cellBars) * 16;
        for (const MelodyNote& n : acid[acidCell(set, variantOfBar(inPhrase, cellBars))]) {
            if (n.step < offset || n.step >= offset + 16) continue;
            // A slide overlaps the next note by a little; other steps are short, as a 303's gate.
            MelodyNote note = n;
            // A slide that reaches into the next bar needs a note there to slide into; where the acid
            // stops at the end of a section it is a plain short note instead.
            if ((note.flags & kNoteSlide) != 0 && note.step - offset + note.len >= 16 && (bp.partsNext & partBit(MelodyPart::Acid)) == 0)
                note.flags = static_cast<uint8_t>(note.flags & ~kNoteSlide);
            const double len = (note.flags & kNoteSlide) ? note.len * 0.25 + 0.03 : std::min<int>(note.len, 2) * 0.25 * 0.55;
            emit(Part::Acid, note, note.step - offset, m.root[kAcidI] + note.rel, len);
        }
    }

    // The line voices -- lead, counter-lead, stab, arp -- in that order of priority, under the register
    // guard (19.09.2026, round "voices"): at every sixteenth their sounding notes lie in disjoint spans
    // at least kRegisterGap semitones apart ("two voices never double the same register at the same
    // time"). The lead is placed as written; each voice after it takes, for the whole bar, the octave
    // that collides least with what is already placed, and gives up the few events that still collide.
    RegisterMap taken;
    const int window = (barInTrack / 8) % 2;
    const int stepBase = (barInTrack % 8) * 16;
    if (has(MelodyPart::Lead)) {
        // The form's octave for the lead only where the whole phrase stays inside B3..A5 (rule 17).
        int shift = 12 * bp.leadOctave;
        for (const MelodyNote& n : lead[window]) {
            const int pitch = m.root[kLeadI] + n.rel + shift;
            if (pitch < m.leadWindowLo || pitch > leadWindowHi(m)) { shift = 0; break; }
        }
        for (const MelodyNote& n : lead[window]) {
            // A note that started in the bar before and still holds here keeps its place in the guard.
            if (n.step + n.len > stepBase && n.step < stepBase + 16) taken.add(n.step - stepBase, n.len, m.root[kLeadI] + n.rel + shift);
            if (n.step < stepBase || n.step >= stepBase + 16) continue;
            // kNoteShort (22.09.2026): the lead's staccato gate, half the written length.
            emit(Part::Lead, n, n.step - stepBase, m.root[kLeadI] + n.rel + shift, n.len * 0.25 * 0.92 * ((n.flags & kNoteShort) ? 0.5 : 1.0));
        }
    }
    if (has(MelodyPart::Counter)) {
        // The answer to the lead (makeCounter), in the lead's register an octave up; the guard may take
        // it one more octave up, or down one, and drops a note that still sits on a lead note.
        std::vector<std::array<int, 3>> ev;
        std::vector<const MelodyNote*> notes;
        for (const MelodyNote& n : counter[window]) {
            // Only the notes that start in this bar: a counter note is cut at the bar line (below), so
            // nothing of it reaches into the next bar, whose guard could not see it.
            if (n.step < stepBase || n.step >= stepBase + 16) continue;
            const int pitch = m.root[kCounterI] + n.rel;
            for (int s = n.step - stepBase; s < std::min(16, n.step - stepBase + n.len); ++s) ev.push_back({ s, pitch, pitch });
            notes.push_back(&n);
        }
        const int shift = bestShift(ev, taken, { 0, 12, -12 }, counterWindowLo(m), counterWindowHi(m));
        for (const MelodyNote* n : notes) {
            const int first = n->step - stepBase, span = std::min(n->len, static_cast<int16_t>(16 - first));
            // The bar's octave first; a single note that still collides tries the octave above and the
            // one below before it gives way, so that an answer keeps its resting note where it can.
            int pitch = -1;
            for (int extra : { 0, 12, -12 }) {
                const int cand = m.root[kCounterI] + n->rel + shift + extra;
                if (cand < counterWindowLo(m) || cand > counterWindowHi(m)) continue;
                bool ok = true;
                for (int s = first; s < first + span; ++s) ok = ok && taken.clear(s, cand, cand);
                if (ok) { pitch = cand; break; }
            }
            if (pitch < 0) continue;
            taken.add(first, span, pitch);
            // The gate by mode (23.09.2026; Form.h, CounterMode). The written note keeps its place in the
            // register guard for its whole span -- the guard is about what *may* sound there. Hocket whips
            // (kCounterStaccato of a sixteenth, at most two sixteenths' worth: the staccato of 20.09.2026);
            // an answer sounds for an eighth at most, at 0.6 of its span; an echo and a timbral note for
            // what they were written.
            const CounterMode cmode = static_cast<CounterMode>(std::clamp(m.counterMode, 0, kNumCounterModes - 1));
            const double gate = cmode == CounterMode::Hocket ? std::min(span, 2) * 0.25 * kCounterStaccato
                              : cmode == CounterMode::Answer ? std::min(span, 2) * 0.25 * 0.6
                              : cmode == CounterMode::Echo ? span * 0.25 * 0.8 : span * 0.25 * 0.95;
            emit(Part::Counter, *n, first, pitch, gate);
        }
    }
    if (has(MelodyPart::Stab) && ((m.stabBars >> (barInTrack % 4)) & 1u) != 0) {
        // Short chord hits on the off-sixteenths of the track's figure (makeStab), over the bar's chord.
        const std::vector<int> chord = stabChordImpl(bp.scale >= 0 ? bp.scale : m.scale, padChordAt(m, bp, barInTrack).degree,
                                                     m.key, m.stabTones);
        const uint16_t mask = m.stabMask[barInTrack % 2];
        std::vector<std::array<int, 3>> ev;
        // A hit sounds for an eighth (two sixteenths), cut at the bar line so the next bar starts clear.
        for (int s = 0; s < kStepsPerBar; ++s) {
            if (((mask >> s) & 1u) == 0 || chord.empty()) continue;
            ev.push_back({ s, chord.front(), chord.back() });
            if (s + 1 < kStepsPerBar) ev.push_back({ s + 1, chord.front(), chord.back() });
        }
        // The bar's octave first: the chord's own (G3 up), an octave under it where the lead sits above
        // (down to D3, the depth rule's floor), or one or two over it where the lead sits below. A hit
        // that still meets a lead or counter note tries the other octaves on its own before it gives way
        // -- the whole chord or nothing.
        const int shift = bestShift(ev, taken, { 0, -12, 12, 24 }, kStabLowest, kStabHighest);
        MelodyNote hit;
        hit.velocity = 108;
        for (int s = 0; s < kStepsPerBar; ++s) {
            if (((mask >> s) & 1u) == 0 || chord.empty()) continue;
            const int span = s + 1 < kStepsPerBar ? 2 : 1;
            int at = -1000;
            for (int extra : { 0, -12, 12, 24, -24 }) {
                const int lo = chord.front() + shift + extra, hi = chord.back() + shift + extra;
                if (lo < kStabLowest || hi > kStabHighest) continue;
                if (!taken.clear(s, lo, hi) || (span == 2 && !taken.clear(s + 1, lo, hi))) continue;
                at = shift + extra;
                break;
            }
            if (at == -1000) continue;
            for (int pitch : chord) {
                taken.add(s, span, pitch + at);
                emit(Part::Stab, hit, s, pitch + at, 0.25 * span);
            }
        }
    }
    if (has(MelodyPart::Arp)) {
        const int c = chordIndexAt(m, barInTrack);
        const std::vector<MelodyNote>& cell = arp[arpCell(set, variantOfBar(barInTrack % 4, 1), c)];
        // G3..G5 whatever the form's octave and the octave jump ask for (rule 14): a shift is played
        // only where the whole cell stays inside, and the cells already span most of the register.
        // Drop 2 (19.09.2026, round "arrangement"): the user's rule "the arp an octave higher" stands above
        // rule 14 there -- the form's octave (BarPlan::arpOctave, set only in drop 2) may take the cell up to
        // G6, the ceiling the register guard already allows beside the lead; the octave jump comes on top
        // only where it still fits.
        const int ceiling = bp.arpOctave > 0 ? kArpOverHighest : kArpHighest;
        auto fits = [&](int sh) {
            for (const MelodyNote& n : cell) {
                const int pitch = m.root[kArpI] + n.rel + sh;
                if (pitch < kArpLowest || pitch > ceiling) return false;
            }
            return true;
        };
        int shift = ((m.arpOctaveJump && (barInTrack / 2) % 2 == 1) ? 12 : 0) + 12 * bp.arpOctave;
        // In drop 2 the octave is the rule's and is kept: a cell too wide for G6 folds its top notes down an
        // octave instead of giving the octave up (below, where the events are built).
        if (!fits(shift)) shift = (bp.arpOctave > 0 || fits(12 * bp.arpOctave)) ? 12 * bp.arpOctave : 0;
        // Which note sounds on which sixteenth: the cell as written, or the polymeter's three-sixteenth
        // cell read at the *absolute* sixteenth of the track (it starts one step later in every bar and
        // comes home every three bars; its high note precesses with it).
        std::vector<std::pair<int, const MelodyNote*>> steps;
        if (m.arpPolymeter && cell.size() == 3) {
            for (int s = 0; s < kStepsPerBar; ++s) steps.emplace_back(s, &cell[static_cast<size_t>((barInTrack + s) % 3)]);
        } else {
            for (const MelodyNote& n : cell) steps.emplace_back(n.step, &n);
        }
        // The arp beside the lead (19.09.2026). Until then the form silenced the arp in every bar the
        // lead played (Composer::restoreArp put it back only where the lead rested). Now the register
        // guard decides, bar by bar, against the lead's (and the counter's and the stab's) real notes:
        // the arp keeps its own octave where that is clear, moves under the lead or over it where that
        // is clearer -- up to G6, past rule 14's G5, because the brief asks for a split "under or over
        // the lead by rule" -- and gives up the single sixteenths that still collide, which puts its
        // accents into the lead's gaps (the interlock). Where nothing else sounds this is the arp as
        // it always was.
        std::vector<std::array<int, 3>> ev;
        for (const auto& st : steps) {
            int pitch = m.root[kArpI] + st.second->rel + shift;
            while (bp.arpOctave > 0 && pitch > ceiling) pitch -= 12;
            ev.push_back({ st.first, pitch, pitch });
        }
        const bool shared = has(MelodyPart::Lead) || has(MelodyPart::Counter) || has(MelodyPart::Stab);
        // In drop 2 the guard may lift the arp further but never take it back under its climax octave.
        // 22.09.2026: **the octave over G5 belongs to the climax.** Outside drop 2 the guard clears the
        // lead within rule 14's own ceiling -- under the lead's lowest note, or interlocked into its
        // rests, both of which it already prefers -- and only drop 2 may go up to G6. Until now both
        // used kArpOverHighest, so in a track whose lead sat in the guard's way the arp was already at
        // the top in drop 1 and drop 2's octave had nowhere to go: the cell was pulled back under the
        // ceiling note by note and the two drops stood level. That is the brief's climax rule not
        // happening at all, and it got much more common once the lead played in every track
        // (21.09.2026) -- measured, the arp rose in drop 2 in 14 of 17 tracks before that and in 9 of
        // 30 after it.
        const int move = !shared ? 0 : bp.arpOctave > 0 ? bestShift(ev, taken, { 0, 12 }, kArpLowest, kArpOverHighest)
                                                        : bestShift(ev, taken, { 0, -12, 12, 24 }, kArpLowest, kArpHighest);
        // The gate (rule 15): kArpGate of a sixteenth; the arp's own release (Params.cpp) finishes it.
        const double gate = 0.25 * kArpGate;
        // 23.09.2026, round "Counter": the lead window moved up into the arp's octaves (Form.h, registerShift),
        // and the sixteenths the bar's best shift still left colliding were *dropped* -- 4 and 13 % holes in
        // the listening seed's arps, measured, in a line whose rule is "continuous sixteenths" (rule 11). A
        // colliding note now moves by an octave on its own -- up, down, two up -- inside the guard's ceiling,
        // and gives way only where no octave is clear. Same pitch class, so the material stands.
        const int guardCeiling = bp.arpOctave > 0 ? kArpOverHighest : kArpHighest;
        for (const auto& st : steps) {
            int pitch = m.root[kArpI] + st.second->rel + shift + move;
            if (!taken.clear(st.first, pitch, pitch)) {
                int alt = -1;
                for (int d : { 12, -12, 24 }) {
                    const int c = pitch + d;
                    if (c >= kArpLowest && c <= guardCeiling && taken.clear(st.first, c, c)) { alt = c; break; }
                }
                if (alt < 0) continue;
                pitch = alt;
            }
            emit(Part::Arp, *st.second, st.first, pitch, gate);
        }
    }
    if (has(MelodyPart::Pad)) {
        // The block's onsets are worked out every bar and only this bar's are written: the conductor
        // composes one bar at a time into the engine's ring, in beat order (22.09.2026, round "Figuren").
        //
        // Which chord, and how long it holds, is padChordAt's (round "Harmonik", the same day): the
        // cores on the pendulum's track-absolute blocks of 4, 8 or 16 bars, a main breakdown on its
        // own progression from its first bar, any other breakdown on one chord for the whole of it. A
        // chord never rings past its section either: a block that straddles a section boundary is
        // cut there, so the breakdown's first chord is not heard under the tail of the groove's last.
        const PadChord chord = padChordAt(m, bp, barInTrack);
        const int barsLeft = std::max(1, std::min(chord.blockBars - chord.barInBlock, bp.sectionBars - bp.barInSection));
        MelodyNote held;
        held.velocity = 90;
        const double barBeats = static_cast<double>(kBeatsPerBar);
        const std::vector<int>& v = (chord.inBreak ? breakVoicing : (chord.secondSet ? voicing2 : voicing))[chord.slot];
        // Intro, breakdown, buildup and outro hold whatever figure the track drew (round "Figuren").
        const bool carpet = bp.type == SectionType::Intro || bp.type == SectionType::Break
                         || bp.type == SectionType::Outro || bp.type == SectionType::Build;
        std::vector<std::pair<int, double>> onsets;
        const int figure = carpet ? static_cast<int>(PadFigure::Held)
                         : bp.type == SectionType::Groove ? m.padFigureGroove : m.padFigure;
        padOnsets(figure, chord.blockBars, onsets);
        const double blockEnd = barsLeft * barBeats;
        std::vector<std::pair<int, double>> here;
        for (const auto& on : onsets) {
            if (on.first / kStepsPerBar != chord.barInBlock) continue;
            double at = (on.first % kStepsPerBar) * 0.25;
            double len = on.second;
            if (at < cut - 1e-9) { len -= cut - at; at = cut; }
            len = std::min(len, blockEnd - at) - 1.0 / 16.0;
            if (len > 1.0 / 16.0) here.emplace_back(static_cast<int>(std::lround(at * 4.0)), len);
        }
        const int step = static_cast<int>(std::lround(cut * 4.0));
        const bool foundation = ctx.foundationBars > 0 && v.size() >= 2;
        // Composer sets foundationBars on the bar a foundation starts -- a block start, or the first
        // bar the floor falls silent inside a held chord -- and nowhere else, so this is that bar.
        if (foundation) {
            // The sub foundation (rule 20): the root under the voicing, only where the form silences
            // kick and bass, fading in with the pad's attack; where the silence ends inside this chord
            // it stops a bar early so the pad's release has taken it away by the time the kick returns.
            // Held whatever the figure, and written on the block's first bar alone: the rule is about
            // *filling* the band under 140 Hz, and a stabbed foundation leaves it empty between hits.
            const std::vector<int> f = foundationVoicing(v);
            const double length = barsLeft * barBeats - 1.0 / 16.0 - cut;
            const double silentBeats = ctx.foundationBars * barBeats;
            const double subLength = ctx.foundationBars > barsLeft
                ? length
                : std::max(0.5 * barBeats, silentBeats - barBeats) - 1.0 / 16.0 - cut;
            MelodyNote sub = held;
            sub.velocity = 118;   // +1 dB at the pad's velocity sensitivity: one voice carrying the register
            emit(Part::Pad, sub, step, f[0], subLength);
            for (size_t i = 1; i < f.size(); ++i)
                for (const auto& on : here) emit(Part::Pad, held, on.first, f[i], on.second);
        } else {
            for (int pitch : v)
                for (const auto& on : here) emit(Part::Pad, held, on.first, pitch, on.second);
        }
    }
    if (has(MelodyPart::Drone) && ctx.droneBars > 0) {
        // The tonic drone (19.09.2026): one held chord per run of the form (Composer.cpp, melodyContext),
        // the root and the fifth, the octave above where the track takes it and the pad is silent. On a
        // silent floor it lies an octave under the pad's register (D2 .. C#3, the exception the pad's sub
        // foundation uses), and it ends droneTail bars before the run does so that its release has died
        // away before the kick returns; where kick and bass play it moves up an octave and back in level
        // (velocity 64 against 110 at a velocity sensitivity of 1: -4.7 dB). The next run's note starts
        // on the run's first beat with the drone's slow attack over this one's release: the cross-fade.
        // 20.09.2026, round "dialogue": the carpet. Where pad or acid share the band (ctx.droneShaded) the
        // drone holds its **root alone** -- the "tiefer, warmer Grundton" of the user's words -- and drops
        // another 2.5 dB (velocity 64 -> 48 at a velocity sensitivity of 1), so that what fills the vacuum
        // is a fundamental and not a second voicing of the chord. With the band to itself it holds root
        // and fifth as before.
        MelodyNote held;
        held.velocity = ctx.droneLow ? 110 : (ctx.droneShaded ? 48 : 64);
        const int root = m.root[kDroneI] + (ctx.droneLow ? 0 : 12);
        std::vector<int> pitches = { root };
        if (!ctx.droneShaded) pitches.push_back(root + 7);
        if (m.droneOctave && !ctx.droneShaded && !has(MelodyPart::Pad)) pitches.push_back(root + 12);
        const double run = ctx.droneBars * static_cast<double>(kBeatsPerBar);
        const double tail = ctx.droneLow ? std::min(run - 0.5 * kBeatsPerBar, ctx.droneTail * static_cast<double>(kBeatsPerBar)) : 0.0;
        const double length = std::max(0.5 * kBeatsPerBar, run - tail) - 1.0 / 16.0 - cut;
        const int step = static_cast<int>(std::lround(cut * 4.0));
        for (int pitch : pitches) emit(Part::Drone, held, step, pitch, length);
    }
}

/**
 * @brief The pad voicing of a chord: root position, the fifth above the root, two upper voices.
 *
 * Rule 19 (18.09.2026): the chord's root is the lowest pad note, between D3 and C#4 -- as low as the
 * depth rule allows while kick and bass play (D3 is 147 Hz) -- with the fifth directly above it, so
 * the low register is full rather than a single note. The two upper voices are chosen from every pair
 * of chord tones above the fifth that completes the triad (the third must be one of them), with no
 * gap wider than an octave, by the smallest total movement from the voicing before -- the voice
 * leading of PLAN 5.7, exact rather than greedy. The bass voice itself moves as the roots do: that is
 * what root position means, and it is what the user asked for ("Auf jeden Fall den Pad-Grundton!").
 * Before this round the pad started at G3 and took any inversion, and the seed the user heard had
 * the root at the bottom of 24 of 96 chords in its first track.
 */
std::vector<int> voiceChord(int scale, int key, int degree, int type, const std::vector<int>* previous)
{
    int iv[4];
    const int n = chordIntervals(static_cast<ChordType>(std::clamp(type, 0, kNumChordTypes - 1)), scale, degree, iv);
    const int rootPc = ((key + scaleDegree(scale, degree)) % 12 + 12) % 12;
    const int root = kPadLowest + ((rootPc - kPadLowest) % 12 + 12) % 12;   // D3 .. C#4
    const int fifth = root + iv[0];
    // The reference without a voicing before: every colour tone at its base position.
    std::vector<int> reference = { root, fifth };
    for (int i = 1; i < n; ++i) reference.push_back(root + iv[i]);
    const std::vector<int>& target = (previous != nullptr && previous->size() >= 2) ? *previous : reference;
    // Every upper voice at its base position or one or two octaves up; the placement that moves least
    // from the target among those with adjacent voices a minor third to an octave apart and nothing
    // over G5. If no placement of all the colour tones fits (a high root under a wide chord), the top
    // one is dropped and the search runs again -- the chord keeps its character before its size.
    for (int use = n; use >= 1; --use) {
        std::vector<int> best;
        int bestCost = 1 << 30;
        const int upper = use - 1;
        int combos = 1;
        for (int i = 0; i < upper; ++i) combos *= 3;
        for (int code = 0; code < combos; ++code) {
            std::vector<int> v = { root, fifth };
            int c = code;
            for (int i = 1; i < use; ++i) { v.push_back(root + iv[i] + 12 * (c % 3)); c /= 3; }
            std::sort(v.begin() + 2, v.end());
            bool ok = v.back() <= kPadHighest;
            for (size_t k = 1; k < v.size() && ok; ++k) { const int gap = v[k] - v[k - 1]; ok = gap >= 3 && gap <= 12; }
            if (!ok) continue;
            int cost = 0;
            const size_t mn = std::min(v.size(), target.size());
            for (size_t k = 0; k < mn; ++k) cost += std::abs(v[k] - target[k]);
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
        if (!best.empty()) return best;
    }
    return { root, fifth };
}

std::vector<int> foundationVoicing(const std::vector<int>& voicing)
{
    if (voicing.size() < 2) return voicing;
    // The sub two octaves under the chord root where that stays at F#1 (46 Hz) or above, else one
    // (22.09.2026, the brief: "immer eine Bassnote in Oktave 1 oder 2 mitfuehren ... bis 50 Hz hinab").
    // Until then it was one octave down from a D3 .. C#4 root, i.e. D2 .. C#3, 73 .. 139 Hz -- an
    // octave short of what the brief calls the fundament. The pad's high pass opens to 40 Hz for it.
    const int sub = voicing[0] - 24 >= kPadFoundationLowest ? voicing[0] - 24 : voicing[0] - 12;
    // Root, fifth and the first colour tone over the sub: at most four voices, as before.
    std::vector<int> out = { sub, voicing[0], voicing[1] };
    if (voicing.size() > 2) out.push_back(voicing[2]);
    return out;
}

std::vector<int> arpHighTones(int scale, int degree, int key, int tones, int& anchor)
{
    // A chord whose root is itself a colour tone (the bII of the Phrygian pendulum) is arpeggiated
    // over the tonic: a b2 on every other sixteenth would be the parked colour tone rule 1 forbids.
    int d = degree;
    if (isColourTone(scale, scaleDegree(scale, d) % 12)) d = 0;
    const int rootPc = ((key + scaleDegree(scale, d)) % 12 + 12) % 12;
    anchor = kArpLowest + ((rootPc - kArpLowest) % 12 + 12) % 12;   // G3 .. F#4
    auto iv = [&](int k) { return scaleDegree(scale, d + k) - scaleDegree(scale, d); };
    // Above the anchor: the octave, the fifth above it and the fifth below it (a perfect one only,
    // so that every high note is at least a fifth above the pedal), and the sus / add9 colour.
    std::vector<int> above = { 12, 12 + iv(4) };
    if (iv(4) >= 7) above.push_back(iv(4));
    if (tones == 0) above.push_back(12 + iv(1));                                  // sus2
    else if (tones == 1) above.push_back(12 + iv(3));                             // sus4
    else { above.push_back(12 + iv(2)); above.push_back(12 + iv(1)); }            // add9: the third and the ninth
    std::vector<int> out;
    for (int a : above) {
        int pitch = anchor + a;
        while (pitch > kArpHighest && pitch - 12 >= anchor + 7) pitch -= 12;
        if (pitch > kArpHighest || pitch < anchor + 7) continue;
        if (isColourTone(scale, ((pitch - key) % 12 + 12) % 12)) continue;
        out.push_back(pitch);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) out.push_back(anchor + 12);
    return out;
}

int voicingMovement(const std::vector<int>& a, const std::vector<int>& b)
{
    int s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += std::abs(a[i] - b[i]);
    return s;
}

void composeSfxBar(const FormPlan& f, double trackBeat, int barInTrack, std::vector<NoteEvent>& out)
{
    // Over the blend (Form.h, FormPlan::overlapTail; 23.09.2026, round "DJ") the next track's intro owns the
    // effects: nothing of this track's is played over its last overlapTail bars. The events were placed when
    // the form was made, before the next track -- and so the blend's length -- existed, which is why the cut
    // is made here and not in makeFormSfx.
    const bool outroEnd = f.count > 0 && f.section[f.count - 1].type == SectionType::Outro;
    if (outroEnd && barInTrack >= f.bars - std::clamp(f.overlapTail, 0, f.bars)) return;
    for (const SfxEvent& s : f.sfx) {
        if (static_cast<int>(std::floor(s.beat / kBeatsPerBar)) != barInTrack) continue;
        NoteEvent e;
        e.beat = trackBeat + s.beat;
        e.length = s.length;
        // Since 19.09.2026 the score carries the part that plays the event -- Sfx, Texture or Vocal
        // (Sfx.h, sfxTypePart) -- instead of leaving the engine and the MIDI export to re-route every
        // effect by its type; and a voice's or the bed's variant rides in the lane (Form.h, SfxEvent).
        e.part = sfxTypePart(static_cast<SfxType>(std::clamp(s.type, 0, kNumSfxTypes - 1)));
        e.lane = s.variant;
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
