/**
 * @file MelodyHarmony.cpp
 * @brief The harmony of a track: its progression, the pad's chords and voicings, the stab's chords.
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
 * @brief A pendulum: the two degrees a track's core alternates between, and each style's taste for it.
 *
 * The user's brief (22.09.2026): "Wenn Akkorde wechseln, nutzen Produzenten fast immer sogenannte
 * Pendel-Harmonien" -- i <-> bII (the Phrygian pendulum, "die Koenigsklasse"), i <-> bVII (subtonic,
 * driving), i <-> iv (the shamanic fourth), and for the more melodic styles i <-> bVI and i <-> v.
 * A pendulum exists only where the mode has its second degree at the interval the name says:
 * bII wants a semitone, bVII a whole tone under the octave, bVI a minor sixth.
 */
struct Pendulum {
    int b;                       ///< the other degree (0-based)
    int semis;                   ///< its interval above the root
    double weight[kNumStyles];   ///< each style's taste for it
};
/** @brief The pendulums and the styles' tastes for them. */
const Pendulum kPendulums[] = {
    //  b  semis   Goa   FullOn  Prog   Dark   HiTech
    { 1,  1,  { 0.50, 0.20, 0.00, 0.50, 0.50 } },   // i <-> bII
    { 6, 10,  { 0.30, 0.40, 0.35, 0.15, 0.30 } },   // i <-> bVII
    { 3,  5,  { 0.20, 0.25, 0.35, 0.30, 0.20 } },   // i <-> iv
    { 5,  8,  { 0.00, 0.10, 0.30, 0.20, 0.00 } },   // i <-> bVI
    { 4,  7,  { 0.00, 0.15, 0.00, 0.00, 0.00 } },   // i <-> v
};
constexpr int kNumPendulums = static_cast<int>(sizeof(kPendulums) / sizeof(kPendulums[0]));   ///< how many pendulums there are

/**
 * @brief Each style's taste for the chord types of Harmony.h, in ChordType order:
 *        triad, sus2, sus4, m7, m9, maj7, quartal.
 *
 * The mode decides which of them exist (chordTypeFits), the pad's rule against rubs which of those it may
 * hold (padAvoidsRubs); this only says what the style reaches for among what is left. The b9 chords that
 * Goa, Dark and Hi-Tech leaned on and the dark styles' m(b5) cluster are gone (Harmony.h, 25.09.2026);
 * their weight went to the suspensions and the fourths, which keep a pad open and unresolved without the
 * rub. The major seventh stays a rare colour, at most 8 %.
 */
const double kTypeWeight[kNumStyles][kNumChordTypes] = {
    //  triad sus2  sus4  m7    m9    maj7  quartal
    { 0.15, 0.20, 0.32, 0.05, 0.00, 0.08, 0.20 },   // Goa
    { 0.20, 0.25, 0.25, 0.20, 0.05, 0.08, 0.05 },   // Full-On
    { 0.05, 0.15, 0.10, 0.30, 0.25, 0.08, 0.20 },   // Progressive
    { 0.10, 0.25, 0.20, 0.05, 0.00, 0.05, 0.35 },   // Dark Forest
    { 0.05, 0.30, 0.15, 0.00, 0.00, 0.05, 0.45 },   // Hi-Tech
};

/** @brief How long a chord holds in the cores: 4, 8 or 16 bars, by style ("oft nur alle 4, 8 oder 16 Takte"). */
const double kBarsWeight[kNumStyles][3] = {
    { 0.20, 0.50, 0.30 },   // Goa
    { 0.40, 0.50, 0.10 },   // Full-On
    { 0.00, 0.40, 0.60 },   // Progressive
    { 0.00, 0.50, 0.50 },   // Dark Forest
    { 0.50, 0.50, 0.00 },   // Hi-Tech
};

/**
 * @brief The classical taste: triads and sevenths first, the suspensions after them -- what a loop progression
 *        is voiced with, so that a bVI or a bVII carries its third and the loop sounds like the progression it is.
 *
 * A loop is the answer to "sonst klingt immer alles schraeg": no suspension it could do without, and the major
 * seventh as rare as in the styles' own taste.
 */
const double kClassicalWeight[kNumChordTypes] = {
    //  triad sus2  sus4  m7    m9    maj7  quartal
    0.50, 0.12, 0.08, 0.20, 0.05, 0.08, 0.03,
};

/**
 * @brief Draws a chord type for @p degree in @p scale from the style's taste, among the types that fit the mode
 *        and rub against nothing (padAvoidsRubs). On the bII nothing does, and the triad stands for the chord
 *        the pad replaces (padChordIntervals).
 */
int drawChordType(Rng& r, int scale, int degree, int styleIdx, bool classical = false)
{
    double w[kNumChordTypes] = {};
    double sum = 0.0;
    for (int t = 0; t < kNumChordTypes; ++t) {
        const ChordType ct = static_cast<ChordType>(t);
        const bool fits = chordTypeFits(scale, degree, ct) && padAvoidsRubs(scale, degree, ct);
        w[t] = fits ? (classical ? kClassicalWeight[t] : kTypeWeight[styleIdx][t]) : 0.0;
        sum += w[t];
    }
    if (sum <= 0.0) {
        // Nothing the style likes fits: the first type that fits at all, and the triad only when
        // even that is wanting -- a chord must never hand the pad a note outside its mode, nor a b9.
        for (int t = 0; t < kNumChordTypes; ++t) {
            const ChordType ct = static_cast<ChordType>(t);
            if (chordTypeFits(scale, degree, ct) && padAvoidsRubs(scale, degree, ct)) return t;
        }
        return static_cast<int>(ChordType::Triad);
    }
    return drawIndex(r, w, kNumChordTypes);
}

/**
 * @brief The minor loops a track may play instead of the pendulum (23.09.2026, round "Harmonie").
 *
 * The user found the ban on classical harmony too strict ("sonst klingt immer alles irgendwie schraeg").
 * The literature keeps psytrance modal at heart but grants the progressive and full-on branches short
 * minor loops (KVR, "Goa/Psytrance and music theory"; Outerverse, "Scales & modes in psytrance"): the
 * aeolian three, its turn, the plagal-dominant swing, and the descending four. Each names its degrees
 * over the four slots and what pitch class each degree has to be for the loop to exist in a mode.
 */
struct Loop {
    int deg[4];         ///< the degrees over the four slots
    int semis[4];       ///< the pitch class each has to be
    const char* name;   ///< its name ("i-bVI-bVII-i")
};
/** @brief The minor loops. */
const Loop kLoops[4] = {
    { { 0, 5, 6, 0 }, { 0, 8, 10, 0 },  "i-bVI-bVII-i" },
    { { 0, 6, 5, 6 }, { 0, 10, 8, 10 }, "i-bVII-bVI-bVII" },
    { { 0, 3, 0, 4 }, { 0, 5, 0, 7 },   "i-iv-i-v" },
    { { 0, 2, 6, 3 }, { 0, 3, 10, 5 },  "i-bIII-bVII-iv" },
};
constexpr int kNumLoops = 4;   ///< how many loops there are
/** @brief How often a style plays a loop instead of a pendulum, and how often the second half changes. */
const float kLoopChance[kNumStyles] = { 0.20f, 0.35f, 0.45f, 0.15f, 0.10f };      // Goa, Full-On, Progressive, Dark, Hi-Tech
const float kSecondHalfChance[kNumStyles] = { 0.30f, 0.30f, 0.40f, 0.25f, 0.20f };   ///< how often the second half of a loop changes, per style

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
 *        track that draws the pendulum plays exactly what it played before loops existed.
 */
int drawProgression(Rng& r, Rng& lr, int scale, int si, int* deg, int* type, int& which)
{
    which = -1;
    double lw[kNumLoops] = {};
    double lsum = 0.0;
    for (int i = 0; i < kNumLoops; ++i) { lw[i] = loopFits(scale, kLoops[i]) ? 1.0 : 0.0; lsum += lw[i]; }
    // The loop's chance against the pendulum's, reweighted by the listener's preferences (Preferences.h); without
    // them the chance itself, so that not a bit of the draw moves.
    float loopChance = kLoopChance[si];
    {
        const double fl = preferenceFactor("harmony", "loop"), fp = preferenceFactor("harmony", "pendulum");
        if (fl != 1.0 || fp != 1.0) loopChance = static_cast<float>(loopChance * fl / (loopChance * fl + (1.0 - loopChance) * fp));
    }
    const bool loop = lr.uniform() < loopChance && lsum > 0.0;
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

/** @brief Draws the track's harmony: its progression (pendulum or loop), the chord length, a second half and the breakdown's chords. */
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

} // namespace mel

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

namespace mel {

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

} // namespace mel

std::vector<int> stabChord(int scale, int degree, int key, int tones) { return stabChordImpl(scale, degree, key, tones); }

void chordTones(int scale, int degree, int out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = scaleDegree(scale, degree + 2 * i) % 12;
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
 * Before 22.09.2026 the pad started at G3 and took any inversion, and the seed the user heard had
 * the root at the bottom of 24 of 96 chords in its first track. The chord is the one the pad may hold
 * (padChordIntervals): no b9, and on the bII the tonic with its minor sixth.
 * @param type the chord type (Harmony.h, kChordTypes)
 */
std::vector<int> voiceChord(int scale, int key, int degree, int type, const std::vector<int>* previous)
{
    int iv[4];
    int rootSemis = 0;
    const int n = padChordIntervals(static_cast<ChordType>(std::clamp(type, 0, kNumChordTypes - 1)), scale, degree, rootSemis, iv);
    const int rootPc = ((key + rootSemis) % 12 + 12) % 12;
    const int root = kPadLowest + ((rootPc - kPadLowest) % 12 + 12) % 12;   // D3 .. C#4
    const int fifth = root + iv[0];
    // The reference without a voicing before: every colour tone at its base position.
    std::vector<int> reference = { root, fifth };
    for (int i = 1; i < n; ++i) reference.push_back(root + iv[i]);
    const std::vector<int>& target = (previous != nullptr && previous->size() >= 2) ? *previous : reference;
    // Every upper voice at its lowest octave over the fifth or one or two octaves up; the placement that
    // moves least from the target among those with adjacent voices a minor third to an octave apart and
    // nothing over G5. If no placement of all the colour tones fits (a high root under a wide chord), the
    // top one is dropped and the search runs again -- the chord keeps its character before its size.
    // (The lowest octave, not the table's: the quartal chord's seventh is listed at 22 and may stand at 10,
    // right over the fifth, which the rule allows and the self test's brute force found.)
    int low[4] = {};
    for (int i = 1; i < n; ++i) { low[i] = iv[i]; while (low[i] - 12 > iv[0]) low[i] -= 12; }
    for (int use = n; use >= 1; --use) {
        std::vector<int> best;
        int bestCost = 1 << 30;
        const int upper = use - 1;
        int combos = 1;
        for (int i = 0; i < upper; ++i) combos *= 3;
        for (int code = 0; code < combos; ++code) {
            std::vector<int> v = { root, fifth };
            int c = code;
            for (int i = 1; i < use; ++i) { v.push_back(root + low[i] + 12 * (c % 3)); c /= 3; }
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

int voicingMovement(const std::vector<int>& a, const std::vector<int>& b)
{
    int s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += std::abs(a[i] - b[i]);
    return s;
}

int bassChordShift(const PadChord& chord, int scale, int bassRoot)
{
    int iv[4], shift = 0;
    padChordIntervals(static_cast<ChordType>(std::clamp(chord.type, 0, kNumChordTypes - 1)), scale, chord.degree, shift, iv);
    if (shift > 6 && bassRoot + shift - 12 >= 28) shift -= 12;   // the nearer octave, never under E1
    return shift;
}

} // namespace phos
