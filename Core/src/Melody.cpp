/**
 * @file Melody.cpp
 * @brief Chords, acid patterns, lead phrases, arps and their schedule.
 */
#include "phos/Melody.h"
#include "phos/Corpus.h"
#include "phos/Dsp.h"
#include "phos/Harmony.h"
#include "phos/Rhythm.h"
#include "phos/Sfx.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr uint64_t kSaltChords = 0x43484F5244000001ull;
constexpr uint64_t kSaltAcid   = 0x4143494400000002ull;
constexpr uint64_t kSaltLead   = 0x4C45414400000003ull;
constexpr uint64_t kSaltArp    = 0x4152500000000004ull;
constexpr uint64_t kSaltSound  = 0x534F554E44000006ull;
constexpr uint64_t kSaltPad    = 0x5041440000000007ull;

using Allowed = std::vector<std::vector<uint8_t>>;

int sym(int rel) { return PitchModel::symbol(std::clamp(rel, kCorpusRelMin, kCorpusRelMax)); }

/**
 * @brief Whether a pitch class is one of the genre's colour tones of @p scale.
 *
 * The flat second (one semitone above the root) and the upper note of an augmented second -- a step of
 * three semitones between two neighbouring degrees, the Hijaz interval of Goa. Easwaran 2004 names both
 * as what makes a psytrance melody sound like one; Farbood's tension model counts them as dissonance,
 * which is one of the four quantities the energy arc moves.
 */
bool isColourTone(int scale, int pcFromRoot)
{
    const int pc = ((pcFromRoot % 12) + 12) % 12;
    if (pc == 1) return true;
    for (int d = 1; d < 7; ++d)
        if (scaleDegree(scale, d) - scaleDegree(scale, d - 1) == 3 && scaleDegree(scale, d) % 12 == pc) return true;
    return false;
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
 * @brief Allowed symbols: scale tones (relative to the key) in [lo, hi] semitones above the part root.
 * @param colour 0 = a plain allowed set (every entry 1, the weight the sampler has always used);
 *               > 0 = weights, with the colour tones lifted by that much over the base of 8.
 */
std::vector<uint8_t> scaleSet(int scale, int rootOffset, int lo, int hi, float colour = 0.0f)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
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

/** @brief Allowed symbols: the chord's tones in [lo, hi]. */
std::vector<uint8_t> chordSet(int scale, int degree, int rootOffset, int lo, int hi)
{
    int pcs[3];
    chordTones(scale, degree, pcs);
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax) continue;
        const int pc = ((rel + rootOffset) % 12 + 12) % 12;
        if (pc == pcs[0] || pc == pcs[1] || pc == pcs[2]) a[static_cast<size_t>(sym(rel))] = 1;
    }
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

/** @brief Draws pitches for a sequence of allowed sets; falls back to the lowest allowed symbol of each. */
std::vector<int> drawPitches(const PitchModel& model, const Allowed& allowed, int ctx2, int ctx1, double temperature, Rng& r)
{
    std::vector<int> syms;
    if (!sampleConstrained(model, allowed, sym(ctx2), sym(ctx1), temperature, Uniform{ &r }, syms)) {
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

void makeAcid(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature)
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
    Allowed allowed;
    for (size_t i = 0; i < on.size(); ++i) allowed.push_back(i == 0 ? single(0) : scaleSet(scale, rootOffset, 0, ambitus));
    const std::vector<int> rels = drawPitches(model, allowed, 0, 0, temperature, r);

    std::vector<MelodyNote>& a = m.acid[0];
    a.clear();
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
        if (r.uniform() < pAccent) n.flags |= kNoteAccent;
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
        for (size_t i = 0; i < a.size(); ++i) vary.push_back(redraw[i] ? scaleSet(scale, rootOffset, 0, ambitus) : single(a[i].rel));
        const std::vector<int> vr = drawPitches(model, vary, 0, 0, temperature, r);
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

void makeLead(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour)
{
    Rng r;
    r.seed(seed ^ kSaltLead);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Lead)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Lead);
    const int rootOffset = ((m.root[1] - key) % 12 + 12) % 12;
    constexpr int lo = -5, hi = 14;
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
    // Constraints of one note: chord tones on strong steps, scale tones elsewhere.
    auto constraintAt = [&](int window, int step) {
        return step % 8 == 0 ? chordSet(scale, chordAtStep(m, window, step), rootOffset, lo, hi)
                             : scaleSet(scale, rootOffset, lo, hi, colour);
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
        if (motif.empty()) motif = drawPitches(model, al, 0, 0, temperature, r);
        {
            // A keeps the motif where it fits this window's chords and redraws the rest.
            Allowed keep;
            for (size_t i = 0; i < motifRhythm.size(); ++i)
                keep.push_back(fits(w, motifRhythm[i], motif[i]) ? single(motif[i]) : al[i]);
            const std::vector<int> a = drawPitches(model, keep, 0, 0, temperature, r);
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
            const std::vector<int> a = drawPitches(model, keep, c2, c1, temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(motifRhythm[i] + 32); rels.push_back(a[i]); }
        }
        // B (bars 4-5): its own rhythm, continuing from A'.
        {
            const std::vector<int> br = rhythm();
            Allowed al2;
            for (int s : br) al2.push_back(constraintAt(w, s + 64));
            const std::vector<int> a = drawPitches(model, al2, rels[rels.size() - 2], rels.back(), temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(br[i] + 64); rels.push_back(a[i]); }
        }
        // A'' (bars 6-7): the motif, its last notes redrawn, ending on a chord tone.
        {
            const size_t n = motifRhythm.size();
            const size_t free = std::max<size_t>(2, (n * 2) / 5);
            Allowed keep;
            for (size_t i = 0; i < n; ++i) {
                const int s = motifRhythm[i] + 96;
                if (i + 1 == n) keep.push_back(chordSet(scale, chordAtStep(m, w, s), rootOffset, lo, hi));
                else if (i + free >= n || !fits(w, s, motif[i])) keep.push_back(constraintAt(w, s));
                else keep.push_back(single(motif[i]));
            }
            const std::vector<int> a = drawPitches(model, keep, rels[rels.size() - 2], rels.back(), temperature, r);
            for (size_t i = 0; i < a.size(); ++i) { steps.push_back(motifRhythm[i] + 96); rels.push_back(a[i]); }
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

void makeArp(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature)
{
    Rng r;
    r.seed(seed ^ kSaltArp);
    const CorpusRole& role = kCorpusRoles[static_cast<int>(CorpusRoleId::Arp)];
    const PitchModel& model = corpusPitchModel(CorpusRoleId::Arp);
    const int rootOffset = ((m.root[2] - key) % 12 + 12) % 12;
    m.arpStyle = r.uniform() < 0.5f ? 0 : 1 + r.below(3);
    m.arpOctaveJump = r.uniform() < 0.3f;
    const int span = r.uniform() < 0.6f ? 12 : 19;
    std::vector<int> on;
    for (int tries = 0; tries < 12; ++tries) {
        on = drawOnsets(r, role, 16);
        if (on.size() >= 10) break;
    }
    if (on.size() < 10) { on.clear(); for (int s = 0; s < 16; ++s) on.push_back(s); }
    for (int c = 0; c < 4; ++c) {
        const std::vector<uint8_t> set = chordSet(scale, m.chordDegree[c], rootOffset, 0, span);
        std::vector<int> tones;
        for (int s = 0; s < kCorpusAlphabet; ++s) if (set[static_cast<size_t>(s)]) tones.push_back(PitchModel::rel(s));
        if (tones.empty()) tones.push_back(0);
        std::vector<int> rels;
        if (m.arpStyle == 0) {
            Allowed al(on.size(), set);
            rels = drawPitches(model, al, tones[0], tones[0], temperature, r);
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

void pitchRange(const std::vector<MelodyNote>& notes, int root, int& lo, int& hi)
{
    for (const MelodyNote& n : notes) { lo = std::min(lo, root + n.rel); hi = std::max(hi, root + n.rel); }
}

/** @brief The pad's gate pattern and the pitch ranges the masking rule works on. */
void makeRangesAndPad(MelodyPlan& m, uint64_t seed)
{
    for (const auto& ph : m.lead) pitchRange(ph, m.root[1], m.leadLo, m.leadHi);
    for (const auto& cell : m.arp) pitchRange(cell, m.root[2], m.arpLo, m.arpHi);
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

MelodyPlan makeMelodyPlan(const ParamStore& p, const StyleProfile& style, uint64_t seed, int key, int scale,
                          bool firstTrack, float colour)
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
    makeAcid(m, key, scale, seed, temperature);
    makeLead(m, key, scale, seed, temperature, colour);
    makeArp(m, key, scale, seed, temperature);
    for (int c = 0; c < 4; ++c) m.padVoicing[c] = voiceChord(scale, m.chordDegree[c], key, c > 0 ? &m.padVoicing[c - 1] : nullptr);
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

void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale,
                      const BarPlan& bp, std::vector<NoteEvent>& out)
{
    (void)scale;
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
        for (const MelodyNote& n : m.acid[variation ? 1 : 0]) {
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
        for (const MelodyNote& n : m.lead[window]) {
            if (n.step < stepBase || n.step >= stepBase + 16) continue;
            emit(Part::Lead, n, n.step - stepBase, m.root[1] + n.rel + 12 * bp.leadOctave, n.len * 0.25 * 0.92);
        }
    }
    if (parts & 4) {
        const int c = chordIndexAt(m, barInTrack);
        const int jump = (m.arpOctaveJump && (barInTrack / 2) % 2 == 1) ? 12 : 0;
        for (const MelodyNote& n : m.arp[c])
            emit(Part::Arp, n, n.step, m.root[2] + n.rel + jump + 12 * bp.arpOctave, 0.25 * 0.5);
    }
    if ((parts & 8) && barInTrack % m.chordBars == 0) {
        MelodyNote held;
        held.velocity = 90;
        // Held to the next chord, a 64th short of it so a repeated pitch takes a fresh voice cleanly.
        // After a cut the chord starts where the cut ends, shortened by as much.
        const double length = m.chordBars * static_cast<double>(kBeatsPerBar) - 1.0 / 16.0 - cut;
        const int step = static_cast<int>(std::lround(cut * 4.0));
        for (int pitch : m.padVoicing[chordIndexAt(m, barInTrack)])
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
