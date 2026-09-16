/**
 * @file Form.cpp
 * @brief Style profiles, the energy arc, the form grammar and the instrumentation matrix.
 */
#include "phos/Form.h"
#include "phos/Dsp.h"
#include "phos/Sfx.h"
#include <algorithm>
#include <cmath>

namespace phos {

// kStyleNames and kArcNames are defined in Params.cpp with the other choice tables, so that the
// parameter table can be built without linking the form.
const char* const kPdbVariantNames[kNumPdbVariants] = { "Full bar", "Half bar", "Beat 4", "Kick on 4" };

// The bass figures a group may end on. They are the rows of Composer's figure table whose *last*
// note differs for every bass pattern (the short patterns play only that note), so two consecutive
// eight-bar groups can never hold the same bass notes. -1 means "no figure", which is the fourth
// distinct value.
const int kGroupFigures[4] = { 0, 1, 3, -1 };

namespace {

constexpr uint64_t kSaltForm    = 0x464F524D00000001ull;
constexpr uint64_t kSaltSection = 0x5345435449000002ull;
constexpr uint64_t kSaltGroup   = 0x47524F5550000003ull;
constexpr uint64_t kSaltSfx     = 0x5346580000000004ull;

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
 * @brief The five style profiles.
 *
 * Tempo of Goa and Full-On measured with Tools/ref_style.py on the user's 40 reference recordings
 * (16.09.2026): Goa median 142.8 BPM over 13 tracks (quartiles 137.9 / 145.0), Full-On 144.6 over 18
 * (quartiles 140.4 / 145.7). The other three have no reference material and keep the tempo windows of
 * the plan (2.1): Progressive 134 to 140, Dark/Forest 146 to 152, Hi-Tech 155 and up.
 *
 * The scale weights follow the plan's harmony section (6.4): Goa leans on Phrygian dominant and double
 * harmonic (the Hijaz colour), Progressive on Dorian and Aeolian, Dark/Forest on Phrygian and harmonic
 * minor. chordExtra adds the pendulum moves the literature round asked for -- i to bII (1 semitone) and
 * i to bVII (10) -- on top of the corpus successions, which measured 0 to 5, 0 to 7 and 0 to 8 as the
 * commonest; the extras do not replace them (see the literature table of 16.09.2026).
 */
const StyleProfile kProfiles[kNumStyles] = {
    // Goa
    { "Goa", 143.0, 4.0,
      { 0.15, 0.20, 0.15, 0.30, 0.15, 0.05 },          // scales: Phrygian dominant and double harmonic lead
      { 0.20, 0.10, 0.70 },                            // bodies: the long second drop
      { 0.0, 0.5, 0.0, 0.0, 0.0, 0.2, 0.0, 0.1, 0.0, 0.0, 0.4, 0.0 },   // i-bII and i-bVII
      { 1.15f, 1.10f, 1.25f, 1.20f }, 0.9f, 0.95f,
      { 0.35, 0.20, 0.25, 0.20 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.26f, 1.0f, 16.0f },
    // Full-On: the default, every multiplier 1, so the knobs play as they are set.
    { "Full-On", 145.0, 4.0,
      { 0.30, 0.25, 0.15, 0.15, 0.05, 0.10 },
      { 0.65, 0.15, 0.20 },
      { 0.0, 0.1, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.1, 0.0 },
      { 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f, 1.0f,
      { 0.30, 0.25, 0.25, 0.20 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.22f, 1.0f, 16.0f },
    // Progressive: flatter form, fewer leads, more pad, Dorian and Aeolian.
    { "Progressive", 137.0, 3.0,
      { 0.35, 0.10, 0.05, 0.05, 0.00, 0.45 },
      { 0.20, 0.70, 0.10 },
      { 0.0, 0.0, 0.0, 0.0, 0.0, 0.3, 0.0, 0.2, 0.0, 0.0, 0.2, 0.0 },
      { 0.9f, 0.55f, 0.8f, 1.3f }, 0.6f, 0.9f,
      { 0.45, 0.25, 0.20, 0.10 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.28f, 0.6f, 16.0f },
    // Dark / Forest: darker modes, less lead, denser percussion, short intros.
    { "Dark Forest", 149.0, 3.0,
      { 0.20, 0.40, 0.25, 0.10, 0.05, 0.00 },
      { 0.50, 0.30, 0.20 },
      { 0.0, 0.4, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.2f, 0.4f, 0.9f, 0.8f }, 1.2f, 1.1f,
      { 0.25, 0.25, 0.35, 0.15 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.18f, 1.3f, 8.0f },
    // Hi-Tech: fastest, busiest, shortest sections.
    { "Hi-Tech", 158.0, 4.0,
      { 0.20, 0.35, 0.25, 0.15, 0.05, 0.00 },
      { 0.55, 0.15, 0.30 },
      { 0.0, 0.3, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.3f, 0.6f, 1.1f, 0.6f }, 1.4f, 1.2f,
      { 0.20, 0.30, 0.35, 0.15 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.16f, 1.4f, 8.0f },
};

/** @brief Control points of the arcs: (t, E) pairs, interpolated with raised cosines. */
struct ArcPoint { double t, e; };
const ArcPoint kWarmUp[]   = { { 0.0, 0.25 }, { 0.35, 0.45 }, { 0.75, 0.70 }, { 1.0, 0.85 } };
const ArcPoint kPeakTime[] = { { 0.0, 0.70 }, { 0.30, 0.95 }, { 0.65, 1.00 }, { 1.0, 0.85 } };
const ArcPoint kMorning[]  = { { 0.0, 0.80 }, { 0.30, 0.70 }, { 0.70, 0.55 }, { 1.0, 0.45 } };
const ArcPoint kClosing[]  = { { 0.0, 0.90 }, { 0.25, 0.75 }, { 0.70, 0.45 }, { 1.0, 0.20 } };
const ArcPoint kFlat[]     = { { 0.0, 0.70 }, { 1.0, 0.70 } };

bool isCore(SectionType t) { return t == SectionType::Groove || t == SectionType::Drop; }

/** @brief The three grammar bodies, as sequences of section types. */
struct Body { int count; SectionType type[8]; };
const Body kBodies[kNumBodies] = {
    // Full-On standard: Groove Build Drop Break Build Drop
    { 6, { SectionType::Groove, SectionType::Build, SectionType::Drop, SectionType::Break, SectionType::Build, SectionType::Drop } },
    // Progressive, flatter: Groove Drop Break Drop Break Drop
    { 6, { SectionType::Groove, SectionType::Drop, SectionType::Break, SectionType::Drop, SectionType::Break, SectionType::Drop } },
    // Goa, long second drop: Intro2 Build Drop Break Build Drop Drop2
    { 7, { SectionType::Intro, SectionType::Build, SectionType::Drop, SectionType::Break, SectionType::Build, SectionType::Drop, SectionType::Drop } },
};

/** @brief Share of the whole track taken by breakdowns. */
double breakShare(const FormPlan& f)
{
    int total = 0, brk = 0;
    for (int i = 0; i < f.count; ++i) {
        total += f.section[i].bars;
        if (f.section[i].type == SectionType::Break) brk += f.section[i].bars;
    }
    return total > 0 ? static_cast<double>(brk) / total : 0.0;
}

/** @brief True while every hard constraint of PLAN 6.2 holds (exposed as formConstraintsHold). */
bool constraintsHoldImpl(const FormPlan& f)
{
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.bars != 8 && s.bars != 16 && s.bars != 32 && s.bars != 64) return false;
        if (isCore(s.type) && s.bars < 16) return false;
        if (s.type == SectionType::Build && s.bars != 8 && s.bars != 16) return false;
        if (s.type == SectionType::Break && s.bars < 16) return false;
        if (s.startBar % 8 != 0) return false;
    }
    const double share = breakShare(f);
    return share >= 0.15 && share <= 0.30;
}

/** @brief Recomputes the start bars and the total after a length changed. */
void relayout(FormPlan& f)
{
    int at = 0;
    for (int i = 0; i < f.count; ++i) { f.section[i].startBar = at; at += f.section[i].bars; }
    f.bars = at;
}

/**
 * @brief Splits @p bars over @p nc cores as a sum of 16, 32 and 64.
 *
 * With a of them 16, b of them 32 and c of them 64: 16a + 32b + 64c = bars and a + b + c = nc, so
 * b + 3c = bars/16 - nc. Taking c as large as the equation allows gives the fewest, longest cores,
 * which is what a Goa body wants; @p out receives the lengths with the long ones first.
 * @return false when no split exists (the length does not fit this many cores)
 */
bool splitCores(int bars, int nc, int* out)
{
    if (nc <= 0 || bars % 16 != 0) return false;
    const int m = bars / 16 - nc;          // extra sixteens to hand out
    if (m < 0 || m > 3 * nc) return false;
    const int c = std::min(nc, m / 3);
    const int b = m - 3 * c;
    const int a = nc - b - c;
    if (a < 0 || b < 0 || b + c > nc) return false;
    int k = 0;
    for (int i = 0; i < c; ++i) out[k++] = 64;
    for (int i = 0; i < b; ++i) out[k++] = 32;
    for (int i = 0; i < a; ++i) out[k++] = 16;
    return k == nc;
}

} // namespace

bool formConstraintsHold(const FormPlan& f) { return constraintsHoldImpl(f); }

const StyleProfile& styleProfile(StyleId id)
{
    const int i = static_cast<int>(id);
    return kProfiles[i < 0 || i >= kNumStyles ? static_cast<int>(StyleId::FullOn) : i];
}

StyleId styleOf(const ParamStore& p)
{
    return static_cast<StyleId>(std::clamp(p.getInt(p.base(Module::Compose) + compose::Style), 0, kNumStyles - 1));
}

ArcId arcOf(const ParamStore& p)
{
    return static_cast<ArcId>(std::clamp(p.getInt(p.base(Module::Compose) + compose::Arc), 0, kNumArcs - 1));
}

double arcEnergy(ArcId arc, double t)
{
    const ArcPoint* pts = kFlat;
    int n = 2;
    switch (arc) {
    case ArcId::WarmUp:   pts = kWarmUp;   n = 4; break;
    case ArcId::PeakTime: pts = kPeakTime; n = 4; break;
    case ArcId::Morning:  pts = kMorning;  n = 4; break;
    case ArcId::Closing:  pts = kClosing;  n = 4; break;
    default: break;
    }
    t = std::clamp(t, 0.0, 1.0);
    for (int i = 0; i + 1 < n; ++i) {
        if (t > pts[i + 1].t) continue;
        const double span = pts[i + 1].t - pts[i].t;
        const double u = span > 0.0 ? (t - pts[i].t) / span : 0.0;
        // Raised cosine: value and slope are continuous at every control point.
        const double w = 0.5 * (1.0 - std::cos(3.141592653589793 * u));
        return pts[i].e + (pts[i + 1].e - pts[i].e) * w;
    }
    return pts[n - 1].e;
}

float typeEnergy(SectionType type)
{
    switch (type) {
    case SectionType::Intro:  return 0.40f;
    case SectionType::Groove: return 0.70f;
    case SectionType::Build:  return 0.75f;   // the start; it rises to the drop's energy
    case SectionType::Drop:   return 1.00f;
    case SectionType::Break:  return 0.30f;
    case SectionType::Outro:  return 0.45f;
    default:                  return 0.50f;
    }
}

FormPlan makeFormPlan(const StyleProfile& s, uint64_t seed, int target, double arcIn, double arcOut)
{
    Rng r;
    r.seed(mixSeed(seed, kSaltForm));
    FormPlan f;
    target = std::clamp((target / 32) * 32, kMinTrackBars, kMaxTrackBars);

    // What the grammar would like: the style's intro length, a mostly long buildup, and breakdowns
    // that together come to the style's share of the track.
    const int wantIntro = r.uniform() < (s.introBars >= 12.0f ? 0.67f : 0.33f) ? 16 : 8;
    const int wantOutro = r.uniform() < 0.6f ? 16 : 8;
    const int wantBuild = r.uniform() < 0.6f ? 16 : 8;
    f.body = drawIndex(r, s.bodyWeight, kNumBodies);
    const Body& body = kBodies[f.body];

    f.count = 0;
    f.section[f.count++].type = SectionType::Intro;
    for (int i = 0; i < body.count; ++i) f.section[f.count++].type = body.type[i];
    f.section[f.count++].type = SectionType::Outro;

    int builds[kMaxSections], nb = 0, breaks[kMaxSections], nbr = 0, cores[kMaxSections], nc = 0;
    int intro2 = -1;
    for (int i = 0; i < f.count; ++i) {
        const SectionType t = f.section[i].type;
        if (t == SectionType::Build) builds[nb++] = i;
        else if (t == SectionType::Break) breaks[nbr++] = i;
        else if (isCore(t)) cores[nc++] = i;
        else if (t == SectionType::Intro && i > 0) intro2 = i;
    }
    const int wantBreak = nbr > 0 ? static_cast<int>(std::lround(s.breakShare * target / nbr)) : 0;

    // The lengths are a small constraint problem: the intro, the outro, the buildups and the
    // breakdowns come from their allowed sets, the cores take what is left, and the total must be the
    // requested length exactly (so that a track boundary always lands on the 32-bar grid, PLAN 6.7,
    // and a rerolled track does not move the tracks after it). The candidate space is at most a few
    // hundred combinations, so it is enumerated and the combination closest to what the grammar wanted
    // wins -- constraint satisfaction rather than a repair loop that might not converge.
    static const int kIntroLens[2] = { 8, 16 }, kBuildLens[2] = { 8, 16 }, kBreakLens[3] = { 16, 32, 64 };
    int bestIntro = 16, bestOutro = 16, bestBuild = 16, bestBreak = 32, bestCores[kMaxSections] = {};
    double bestScore = 1e30;
    bool found = false;
    for (int ii = 0; ii < 2; ++ii)
        for (int oi = 0; oi < 2; ++oi)
            for (int bi = 0; bi < 2; ++bi)
                for (int ki = 0; ki < 3; ++ki) {
                    const int in = kIntroLens[ii], ou = kIntroLens[oi], bu = kBuildLens[bi], br = kBreakLens[ki];
                    const int fixed = in + ou + nb * bu + nbr * br + (intro2 >= 0 ? 8 : 0);
                    int lens[kMaxSections];
                    if (!splitCores(target - fixed, nc, lens)) continue;
                    const double share = nbr * br / static_cast<double>(target);
                    if (share < 0.15 || share > 0.30) continue;
                    const double score = std::abs(in - wantIntro) + std::abs(ou - wantOutro)
                                       + nb * std::abs(bu - wantBuild) + 0.5 * nbr * std::abs(br - wantBreak);
                    if (score >= bestScore) continue;
                    bestScore = score;
                    bestIntro = in;
                    bestOutro = ou;
                    bestBuild = bu;
                    bestBreak = br;
                    for (int k = 0; k < nc; ++k) bestCores[k] = lens[k];
                    found = true;
                }
    f.section[0].bars = bestIntro;
    f.section[f.count - 1].bars = bestOutro;
    if (intro2 >= 0) f.section[intro2].bars = 8;
    for (int k = 0; k < nb; ++k) f.section[builds[k]].bars = bestBuild;
    for (int k = 0; k < nbr; ++k) f.section[breaks[k]].bars = bestBreak;
    // The long cores go where the body wants them: the Goa body ends on its long second drop, the
    // others put their weight on the first.
    for (int k = 0; k < nc; ++k) f.section[cores[f.body == 2 ? nc - 1 - k : k]].bars = found ? bestCores[k] : 16;
    relayout(f);
    if (!found) {
        // No combination fits: keep the cores at their floor and give the difference to the outro. The
        // enumeration covers every target between kMinTrackBars and kMaxTrackBars for all three bodies
        // (there is a self-test check for that), so this is a guard, not a path.
        f.section[f.count - 1].bars += target - f.bars;
        relayout(f);
    }

    // Energies from the arc, and the per-section decisions that belong to the form.
    Rng d;
    d.seed(mixSeed(seed ^ kSaltSection, 0));
    for (int i = 0; i < f.count; ++i) {
        Section& sec = f.section[i];
        const double t0 = f.bars > 0 ? static_cast<double>(sec.startBar) / f.bars : 0.0;
        const double t1 = f.bars > 0 ? static_cast<double>(sec.startBar + sec.bars) / f.bars : 1.0;
        const double a0 = arcIn + (arcOut - arcIn) * t0, a1 = arcIn + (arcOut - arcIn) * t1;
        // The arc scales the type's nominal energy; at arc 0 a section keeps 55 % of it, so the order
        // drop > buildup > groove > breakdown survives every arc.
        auto scaled = [](float nominal, double arc) { return static_cast<float>(nominal * (0.55 + 0.45 * arc)); };
        sec.energy = scaled(typeEnergy(sec.type), a0);
        sec.energyTo = scaled(typeEnergy(sec.type), a1);
        if (sec.type == SectionType::Build) {
            // A buildup rises from the section before it to the drop that follows.
            // A buildup starts where the section before it ended and arrives at the drop's energy, so
            // that everything the energy drives -- the gain, the filter arcs -- really rises through it
            // (Solberg and Dibben 2019: the rising middle of the U).
            const float from = i > 0 ? f.section[i - 1].energyTo : 0.5f;
            const float to = i + 1 < f.count ? scaled(typeEnergy(f.section[i + 1].type), a1) : 1.0f;
            sec.energy = from;
            sec.energyTo = to;
            sec.pdbVariant = drawIndex(d, s.pdbWeight, kNumPdbVariants);
        }
        if (sec.type == SectionType::Break && i > 0 && isCore(f.section[i - 1].type))
            sec.cutBeats = d.uniform() < 0.5f ? 1.0f : 2.0f;   // Grosz et al.: 1 to 3 s
    }
    return f;
}

int sectionOfBar(const FormPlan& f, int barInTrack)
{
    for (int i = f.count - 1; i >= 0; --i) if (barInTrack >= f.section[i].startBar) return i;
    return 0;
}

/** @brief planBar, with one level of look-ahead for the parts of the following bar. */
static BarPlan planBarImpl(const FormPlan& f, const PartAvailability& a, const uint64_t* sectionSeed, int barInTrack, bool lookAhead)
{
    BarPlan bp;
    const int si = sectionOfBar(f, barInTrack);
    const Section& s = f.section[si];
    const int b = std::max(0, barInTrack - s.startBar);
    bp.index = si;
    bp.type = s.type;
    bp.barInSection = b;
    bp.group = barInTrack / 8;
    const double u = s.bars > 1 ? static_cast<double>(b) / (s.bars - 1) : 0.0;
    bp.energy = static_cast<float>(s.energy + (s.energyTo - s.energy) * u);
    bp.pdbVariant = s.pdbVariant;

    // Section-level draws, in a fixed order, so the same bar always gets the same answer.
    const uint64_t ss = sectionSeed[std::clamp(si, 0, kMaxSections - 1)];
    Rng rs;
    rs.seed(mixSeed(ss ^ kSaltSection, 0));
    // The kick joins between bar 5 and bar 9, and never after the intro is over (an eight-bar intro
    // cannot wait until bar 9).
    const int introKickBar = std::min(4 + rs.below(5), std::max(1, s.bars - 1));
    // A drop brings everything back at once (the instrumentation matrix of PLAN 6.1), which is also
    // what makes Solberg and Dibben's Track 2 rule hold: after the drop the spectrum must be at least
    // as full as it was before the break. Every other section may leave a voice out.
    const bool drop = s.type == SectionType::Drop;
    const bool drawAcid = rs.uniform() < 0.9f, drawLead = rs.uniform() < 0.85f, drawArp = rs.uniform() < 0.8f;
    const bool useAcid = a.part[0] && (drop || drawAcid);
    const bool useLead = a.part[1] && (drop || drawLead);
    const bool useArp = a.part[2] && (drop || drawArp);
    const bool breakLead = rs.uniform() < 0.5f;     // a breakdown keeps the lead or the arp, not both
    const bool padExtra = rs.uniform() < 0.45f;     // pads join a section that already has lead or acid
    const bool gate = rs.uniform() < 0.35f;
    const int layerStep = 4 + 4 * rs.below(2);      // outro: one layer leaves every 4 or 8 bars

    // Group-level change: inside a section no eight-bar group repeats its predecessor's change
    // (Easwaran 2004: something new every four or eight bars; Butler's hypermetre).
    const int groupInSection = b / 8;
    Rng rg;
    rg.seed(mixSeed(ss ^ kSaltGroup, static_cast<uint64_t>(groupInSection)));
    // The figure is walked from the section's first group, because "differs from the group before"
    // has to compare against what that group really played: a group whose draw was already pushed
    // aside would otherwise be compared against its raw draw, and one pair in sixteen would repeat
    // (measured: 7 of 91).
    int figure = 0;
    for (int g = 0; g <= groupInSection; ++g) {
        Rng rr;
        rr.seed(mixSeed(ss ^ kSaltGroup, static_cast<uint64_t>(g)));
        int draw = rr.below(4);
        if (g > 0 && draw == figure) draw = (draw + 1) % 4;
        figure = draw;
    }
    rg.below(4);                                    // the same draw the walk above made for this group
    const int layerNudge = rg.below(3) - 1;         // -1, 0 or +1 layer for this group

    const int maxLayers = std::max(1, a.percLayers);
    uint8_t parts = 0;
    int layers = maxLayers;

    switch (s.type) {
    case SectionType::Intro:
        // Percussion layered up from nothing; the kick joins between bar 5 and 9; pad or atmosphere.
        layers = std::min(maxLayers, b / 4);
        bp.kickBeats = b >= introKickBar ? 0xF : 0;
        bp.bassBeats = b >= introKickBar ? 0xF : 0;
        if (a.part[3]) parts |= 8;
        bp.fills = false;
        break;
    case SectionType::Groove:
        // The first core: the groove in its essential form, the lead still held back.
        layers = std::min(maxLayers, 2 + groupInSection);
        if (useAcid) parts |= 1;
        if (useArp && b >= 8) parts |= 4;
        if (useLead && b >= s.bars / 2 && s.bars >= 32) parts |= 2;
        break;
    case SectionType::Build: {
        // Layers return one per four bars; the snare roll fills the last four bars; hats denser.
        layers = std::min(maxLayers, 1 + b / 4);
        if (useAcid) parts |= 1;
        if (useArp && b >= s.bars / 2) parts |= 4;
        bp.hatsDense = b >= s.bars / 2;
        if (b >= s.bars - 4) bp.rollBar = 4 - (s.bars - b);
        if (b == s.bars - 1) {
            // The pre-drop break. Variants: whole bar, half bar, beat 4 only, kick alone on beat 4.
            bp.pdb = true;
            static const uint8_t kKickMask[kNumPdbVariants] = { 0x0, 0x3, 0x7, 0x8 };
            static const uint8_t kBassMask[kNumPdbVariants] = { 0x0, 0x3, 0x7, 0x0 };
            bp.kickBeats = kKickMask[std::clamp(s.pdbVariant, 0, kNumPdbVariants - 1)];
            bp.bassBeats = kBassMask[std::clamp(s.pdbVariant, 0, kNumPdbVariants - 1)];
            parts = 0;
            layers = 0;
        }
        break;
    }
    case SectionType::Drop:
        // Everything back at once.
        layers = maxLayers;
        if (useAcid) parts |= 1;
        if (useLead) parts |= 2;
        if (useArp) parts |= 4;
        break;
    case SectionType::Break:
        // Solberg and Dibben 2019: the sudden removal of bass and bass drum. Pads carry it, with one
        // thinned melodic voice; a hat returns in the second half.
        bp.kickBeats = 0;
        bp.bassBeats = 0;
        layers = b >= s.bars / 2 ? 1 : 0;
        if (a.part[3]) parts |= 8;
        if (b >= s.bars / 4) parts |= breakLead ? (useLead ? 2 : 0) : (useArp ? 4 : 0);
        bp.fills = false;
        if (b == 0) bp.cutBeats = s.cutBeats;
        break;
    case SectionType::Outro:
        // Layers leave one per four or eight bars; kick and bass hold on for the DJ.
        layers = std::max(0, maxLayers - 1 - b / layerStep);
        if (useAcid && b < s.bars / 2) parts |= 1;
        if (a.part[3]) parts |= 8;
        bp.fills = false;
        break;
    default: break;
    }

    // Density (Farbood): the energy nudges the layer count, and the group's own change moves it again.
    if (s.type == SectionType::Groove || s.type == SectionType::Drop) {
        layers = std::clamp(layers + layerNudge, std::min(2, maxLayers), maxLayers);
        if (s.energy < 0.6f) layers = std::max(std::min(2, maxLayers), layers - 1);
    }
    bp.percLayers = std::clamp(layers, 0, maxLayers);

    // Pads carry what has neither lead nor acid, and join some of the rest.
    if (a.part[3] && s.type != SectionType::Build && s.type != SectionType::Intro) {
        if ((parts & 3) == 0 || padExtra) parts |= 8;
    }
    bp.padGate = (parts & 8) != 0 && (parts & 3) != 0 && gate;

    // Register (Farbood): a high-energy section lifts the arp an octave, a low-energy one drops the
    // lead, always within the depth rule (arp from G3, lead from B3).
    // The lead's octave moves first, because the masking rule below has to see where the lead really
    // sits; it only moves down where the depth rule still holds (a lead never sounds under B3).
    // The register is a decision of the section, not of the bar: inside a buildup the energy rises,
    // and a lead that changed octave halfway through would break the masking rule the section was
    // planned with. So both registers read the section's own energy.
    const int leadOct = (s.energy < 0.45f && a.leadLo >= 71) ? -1 : 0;
    const int leadLo = a.leadLo + 12 * leadOct, leadHi = a.leadHi + 12 * leadOct;
    int arpShift = 0;
    if (s.energy >= 0.92f) arpShift = 1;
    // The masking rule, decided for the whole section rather than bar by bar: octaves up until the
    // arp's range and the lead's overlap by at most two semitones, or the arp sits the section out.
    // It has to hold for the section, because a section can bring the arp in before the lead, and an
    // arp that changed octave when the lead joined would mask the bars before that.
    if (useLead && useArp && a.part[1] && a.part[2]) {
        while (a.arpLo + 12 * arpShift < leadHi - 2 && a.arpHi + 12 * arpShift > leadLo + 2) ++arpShift;
        if (a.arpHi + 12 * arpShift > 100) parts &= static_cast<uint8_t>(~4);
    }
    bp.arpOctave = static_cast<int8_t>(arpShift);
    bp.leadOctave = static_cast<int8_t>(leadOct);
    bp.parts = parts;

    // The group's figure: at beat 1 of the last bar of every eight-bar group of a core. The figures
    // differ in their last note for every bass pattern, so no two consecutive groups are identical.
    if (isCore(s.type) && b % 8 == 7 && (bp.bassBeats & 0x2) != 0) bp.groupFigure = kGroupFigures[figure];
    // What the next bar plays, so that an acid slide at the end of a bar knows whether there is still
    // a note to slide into (the last bar of an acid section must not slide into silence).
    if (lookAhead && barInTrack + 1 < f.bars) bp.partsNext = planBarImpl(f, a, sectionSeed, barInTrack + 1, false).parts;
    return bp;
}

BarPlan planBar(const FormPlan& f, const PartAvailability& a, const uint64_t* sectionSeed, int barInTrack)
{
    return planBarImpl(f, a, sectionSeed, barInTrack, true);
}

void makeFormSfx(FormPlan& f, uint64_t seed, float amount)
{
    Rng r;
    r.seed(mixSeed(seed ^ kSaltSfx, 0));
    f.sfx.clear();
    auto add = [&](double beat, float length, SfxType type) {
        SfxEvent e;
        e.beat = std::max(0.0, beat);
        e.length = length;
        e.type = static_cast<int>(type);
        f.sfx.push_back(e);
    };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * kBeatsPerBar;
        const double end = static_cast<double>(s.startBar + s.bars) * kBeatsPerBar;
        if (s.type == SectionType::Build) {
            // The riser climbs over the last eight bars and arrives on the drop; the formant shot is
            // the pre-drop "Abriss" on beat 4 of the PDB bar; the sweep falls into the drop, which
            // Solberg and Dibben found to be the marker listeners react to.
            const double rise = std::min(8.0, static_cast<double>(s.bars)) * kBeatsPerBar;
            if (r.uniform() < amount) add(end - rise, static_cast<float>(rise), SfxType::Riser);
            if (r.uniform() < amount) add(end - 1.0, 0.5f, SfxType::FormantShot);
            if (r.uniform() < 0.8f * amount) add(end - 2.0 * kBeatsPerBar, 2.0f * kBeatsPerBar, SfxType::Sweep);
            if (r.uniform() < amount) add(end, 4.0f, SfxType::Impact);
        } else if (s.type == SectionType::Break) {
            if (r.uniform() < amount) add(start, 4.0f * kBeatsPerBar, SfxType::Downlifter);
            if (r.uniform() < 0.6f * amount) add(start + 2.0 * kBeatsPerBar, 2.0f * kBeatsPerBar, SfxType::ReverseSwell);
        } else if (s.type == SectionType::Drop && i > 0 && f.section[i - 1].type == SectionType::Break) {
            // A drop straight out of a breakdown (the flat Progressive body) gets its own marker.
            if (r.uniform() < amount) add(start - 2.0 * kBeatsPerBar, 2.0f * kBeatsPerBar, SfxType::Sweep);
            if (r.uniform() < amount) add(start, 4.0f, SfxType::Impact);
        } else if (s.type == SectionType::Outro) {
            // The sweep over the last eight bars masks the key change into the next track (PLAN 6.7).
            add(end - 8.0 * kBeatsPerBar, 8.0f * kBeatsPerBar, SfxType::Sweep);
        }
    }
    std::stable_sort(f.sfx.begin(), f.sfx.end(), [](const SfxEvent& a, const SfxEvent& b) { return a.beat < b.beat; });
}

} // namespace phos
