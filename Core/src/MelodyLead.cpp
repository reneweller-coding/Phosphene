/**
 * @file MelodyLead.cpp
 * @brief The lead and the counter-lead: rhythm cells, archetypes, the critic, and the answer.
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

/** @name The lead's register (rule 17): a soft centre in the window C4 .. G4 (Melody.h)
 *  A sigma of 5 semitones would be flat across a window of seven and leave the register weight with
 *  nothing to say; at 2.5 the weight falls to 0.6 at the window's edges (20.09.2026, "dialogue").
 *  @{ */
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
 * offset (CellOp::EndCadence). The arcs are what ComposerControls.cpp writes onto the lead's cutoff, bar by
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
 * out different from the serial one (testProbeSchedule, 22.09.2026).
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
 * three times running. The weights are the ones that made the seeds of docs/rounds/2026-09.md sound as the
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
 * What the user heard until 22.09.2026 -- "eher zufällig" -- was eight bars drawn note by note
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
        // The Phrygian family's b2 is a degree of the lead's own (Harmony.h, isLineColourTone), a little rarer than
        // the others; beats 1 and 3 still take a chord tone (constraintAt).
        scaleW[pc] = isLineColourTone(scale, pc) ? 0.0 : (pc == 0 || pc == 7 ? 1.3 : (pc == 1 ? 0.8 : 1.0));
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
    // The nearest legal pitch to `rel` at `step`: a scale tone, no colour tone (the structural b2 is none,
    // isLineColourTone), a chord tone on a strong step.
    // `dir` (the bar's shift) breaks ties towards the transposition, so a snapped strong step does not undo it.
    auto legal = [&](int step, int rel, int dir) {
        if (fits(step, rel) && !isLineColourTone(scale, pcOf(rel))) return rel;
        const int first = dir >= 0 ? 1 : -1;
        for (int d = 1; d <= 12; ++d) {
            for (int c : { rel + first * d, rel - first * d })
                if (c >= lo && c <= hi && inScale(scale, pcOf(c)) && !isLineColourTone(scale, pcOf(c)) && (step % 8 != 0 || isChord(pcOf(c)))) return c;
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
        double archWeight[kNumLeadArchetypes];   // the style's weights times the listener's preferences (Preferences.h)
        for (int a = 0; a < kNumLeadArchetypes; ++a) archWeight[a] = ls.archetype[a] * preferenceFactor("lead.archetype", kLeadArchetypeNames[a]);
        m.leadArchetype[w] = drawIndex(r, archWeight, kNumLeadArchetypes);
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
                            // from -- but never a colour tone (legal() would snap it straight back onto the tonic:
                            // three tonics in a row, measured), so the other side or a farther step where the nearer
                            // ones are colour; in Double Harmonic 3 and 7 are, and a tonic at the window's floor left
                            // nothing, so the search goes out to three steps and, failing that, takes the nearest
                            // legal tone above. The Phrygian family's b2 is no colour tone for the lead
                            // (isLineColourTone): F - E is the Phrygian cadence itself.
                            const int side = bar.rels[bn - 3] >= tonic ? 1 : -1;
                            int approach = tonic;
                            for (int cand : { side, -side, 2 * side, -2 * side, 3 * side, -3 * side }) {
                                const int c = transposeRel(scale, rootOffset, tonic, cand);
                                if (c >= lo && c <= hi && !isLineColourTone(scale, pcOf(c))) { approach = c; break; }
                            }
                            if (approach == tonic) {
                                for (int d = 1; d <= 12 && approach == tonic; ++d)
                                    for (int c : { tonic + d, tonic - d })
                                        if (approach == tonic && c >= lo && c <= hi && inScale(scale, pcOf(c)) && !isLineColourTone(scale, pcOf(c))) approach = c;
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
                if (!placeColour(scale, rootOffset, lo, hi, hi, bar.rels[i], cr, c, t, true)) continue;
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
            if (!inScale(scale, pcOf(rel)) || isLineColourTone(scale, pcOf(rel)) || rel == from || rel == after) continue;
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

} // namespace mel

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

} // namespace phos
