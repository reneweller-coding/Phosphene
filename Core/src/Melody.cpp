/**
 * @file Melody.cpp
 * @brief The line machinery (symbols, allowed sets, the constrained draw and its repair), the melody plan, and the
 *        bars of the melodic parts. The parts' makers are in MelodyHarmony.cpp, MelodyLead.cpp and MelodyParts.cpp.
 */
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
#include "MelodyInternal.h"

#include "Salts.h"
using namespace phos::salts::melody;   // the melody's seed salts (Salts.h)

namespace phos {

using namespace mel;

namespace mel {

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

/**
 * @brief The colour tones of a mode as pitch classes above the tonic (Harmony.h, isColourTone) -- for the acid and
 *        the lead (@p line) without the Phrygian family's b2, which is a degree of theirs (isLineColourTone).
 */
std::vector<int> colourPcs(int scale, bool line)
{
    std::vector<int> out;
    for (int d = 0; d < 7; ++d) {
        const int pc = scaleDegree(scale, d) % 12;
        if (line ? isLineColourTone(scale, pc) : isColourTone(scale, pc)) out.push_back(pc);
    }
    return out;
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
 * @param scale      index into kScaleSteps
 * @param degree     the chord's scale degree
 * @param rootOffset the part root's distance from the key, in semitones
 * @param lo,hi      the interval range to the part root
 * @param tension    the tension curve's tilt at this position (tensionAt); 0 leaves the set flat
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

/** @brief An allowed-symbol set that admits the one interval @p rel. */
std::vector<uint8_t> single(int rel)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    a[static_cast<size_t>(sym(rel))] = 1;
    return a;
}

/**
 * @brief Where a colour tone and its resolution stand: the colour pitch nearest to @p near inside
 *        [lo, hi], and the tonic nearest to it inside [lo, resHi].
 *
 * With @p line set (the acid's or the lead's slot) the Phrygian family's b2 is a degree there, not a colour
 * (25.09.2026), so a slot of theirs in plain Phrygian has nothing to place.
 * @return false where the mode has no colour tone or neither fits the window
 */
bool placeColour(int scale, int rootOffset, int lo, int hi, int resHi, int near, Rng& cr, int& colourRel, int& tonicRel, bool line)
{
    const std::vector<int> pcs = colourPcs(scale, line);
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
 * @param model   the corpus Markov model (when @p src has no neural model)
 * @param role    the corpus role whose statistics are drawn
 * @param src     the neural model and its conditioning, if any
 * @param allowed the allowed symbols per position
 * @param ctx2,ctx1 the two symbols before the first position
 * @param temperature the sampler's temperature
 * @param r       the stream the draw consumes
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
 * @brief Redraws single notes of a drawn line until it keeps @p rules.
 *
 * Each repair redraws one note from the same model, with every other note of the line fixed -- the
 * constrained sampler then draws that note from the model's distribution given both its neighbours,
 * which is how the variations of the acid have always been drawn. The note's own set loses exactly
 * what the rule forbids there (the pitch of a run, or the pitch classes a window already has too
 * many or too few of), so the learned model still chooses; it only chooses inside the rule. The
 * rules win where the model disagrees: the acid corpus plays two pitch classes a bar and repeats
 * 59 % of its notes (docs/rounds/2026-09.md, 18.09.2026), and that is exactly what the brake is there for.
 *
 * @param fixed positions no repair may touch (the tonic that opens a cell, colour slots and their
 *              resolutions, the fifth a lead rests on, the notes a variation keeps)
 * @param model,role,src,allowed,steps,bars,ctx2,ctx1,temperature as for drawPitches()
 * @param rules the line's rules (LineRules)
 * @param r     the stream the repairs consume
 * @param rels  the line, intervals to the part root, repaired in place
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

/** @brief The median index of a histogram of @p n bins (12 when it is empty). */
int median(const uint32_t* counts, int n)
{
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += counts[i];
    uint64_t acc = 0;
    for (int i = 0; i < n; ++i) { acc += counts[i]; if (acc * 2 >= total && total > 0) return i; }
    return 12;
}

// ---------------------------------------------------------------------------------------------

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
} // namespace mel

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
    // is not a "line" for the presence match or for a drop's brightness (ComposerLevels.cpp, matchPresence and
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
        double modeWeight[kNumCounterModes];   // times the listener's preferences (Preferences.h)
        for (int a = 0; a < kNumCounterModes; ++a) modeWeight[a] = style.lead.counterMode[a] * preferenceFactor("counter.mode", kCounterModeFeatureNames[a]);
        m.counterMode = counterKnob > 0 ? counterKnob - 1 : drawIndex(cm, modeWeight, kNumCounterModes);
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

namespace mel {

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
 * @param taken  the pitches the other voices already hold, per step
 * @param shifts the octave shifts to try, in semitones
 * @param lo,hi  the lowest and highest pitch the shifted events may reach
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

} // namespace mel

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

} // namespace phos
