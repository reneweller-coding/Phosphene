/**
 * @file MelodyParts.cpp
 * @brief The acid, the arp, the pad's figure, the stab's hits and the drone.
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
    // The Phrygian family's b2 is a degree of the acid's own (Harmony.h, isLineColourTone): the F of the E - F - E
    // riff, on any step and held too, a little less likely than the third. (The acid's colour slots no longer place
    // the b2 on top of that -- placeColour's `line` -- which with the neural model, whose Phrygian row lifts the b2
    // eightfold, put it on 29 % of the acid's notes.)
    if (inScale(scale, 1)) { core[1] = 2.0; rest[1] = 1.0; }
    for (int pc = 0; pc < 12; ++pc)
        if (isLineColourTone(scale, pc)) core[pc] = rest[pc] = jump[pc] = 0.0;
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
            if (!placeColour(scale, rootOffset, lo, hi, nx == 0 ? 0 : jumpHi, 0, cr, c, t, true)) continue;
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
            // A colour tone is never held: one sixteenth, whatever it follows (rule 1). The structural b2 is none
            // (isLineColourTone) and keeps its length and its slide.
            if (isLineColourTone(scale, pcOf(note.rel))) { note.len = 1; note.flags = static_cast<uint8_t>(note.flags & ~kNoteSlide); }
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
    double arpWeight[kNumArpStyles];   // times the listener's preferences (Preferences.h)
    for (int a = 0; a < kNumArpStyles; ++a) arpWeight[a] = kArpStyleWeights[a] * preferenceFactor("arp.style", kArpStyleFeatureNames[a]);
    m.arpStyle = drawIndex(r, arpWeight, kNumArpStyles);
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
            if (placeColour(scale, rootOffset, lo, hi, hi, aRel, cr, glintRel, res, false) && res == aRel) {
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
 * @param notes the notes, as intervals over @p root
 * @param root  the part root, a MIDI note
 * @param lo,hi the range, widened in place
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

} // namespace mel

const PitchModel& corpusPitchModel(CorpusRoleId role)
{
    static const PitchModel acidModel(CorpusRoleId::Acid), leadModel(CorpusRoleId::Lead), arpModel(CorpusRoleId::Arp);
    return role == CorpusRoleId::Acid ? acidModel : (role == CorpusRoleId::Lead ? leadModel : arpModel);
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

} // namespace phos
