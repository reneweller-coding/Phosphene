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
constexpr uint64_t kSaltMode    = 0x4D4F44450000005ull;   ///< the section's borrowed mode (16.09.2026)
constexpr uint64_t kSaltRide    = 0x5249444500000006ull;   ///< the section's macro ride (16.09.2026)

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
 *
 * interchangeWeight and interchangeChance are the modal interchange of 16.09.2026 (Form.h). They are
 * deliberately *not* the key-journey weights above: the journey moves between tracks and may change
 * the tonic with the mode, while an interchange happens over a fixed tonic pedal inside one track, so
 * Goa borrows the Hijaz modes but Progressive borrows almost nothing and Hi-Tech borrows only the
 * darker minors. Progressive's chance is small on purpose -- its literature is a genre of one mode
 * held for eight minutes -- rather than zero, so the path is exercised in every style.
 */
const StyleProfile kProfiles[kNumStyles] = {
    // Goa
    { "Goa", 143.0, 4.0,
      { 0.15, 0.20, 0.15, 0.30, 0.15, 0.05 },          // scales: Phrygian dominant and double harmonic lead
      { 0.20, 0.10, 0.70 },                            // bodies: the long second drop
      { 0.0, 0.5, 0.0, 0.0, 0.0, 0.2, 0.0, 0.1, 0.0, 0.0, 0.4, 0.0 },   // i-bII and i-bVII
      { 1.15f, 1.10f, 1.25f, 1.20f }, 0.9f, 0.95f,
      { 0.35, 0.20, 0.25, 0.20 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.26f, 1.0f, 16.0f,
      { 0.15, 0.25, 0.10, 0.30, 0.15, 0.05 }, 0.45f },   // interchange: the Hijaz modes at the peak
    // Full-On: the default, every multiplier 1, so the knobs play as they are set.
    { "Full-On", 145.0, 4.0,
      { 0.30, 0.25, 0.15, 0.15, 0.05, 0.10 },
      { 0.65, 0.15, 0.20 },
      { 0.0, 0.1, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.1, 0.0 },
      { 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f, 1.0f,
      { 0.30, 0.25, 0.25, 0.20 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.22f, 1.0f, 16.0f,
      { 0.30, 0.30, 0.15, 0.15, 0.05, 0.05 }, 0.30f },
    // Progressive: flatter form, fewer leads, more pad, Dorian and Aeolian.
    { "Progressive", 137.0, 3.0,
      { 0.35, 0.10, 0.05, 0.05, 0.00, 0.45 },
      { 0.20, 0.70, 0.10 },
      { 0.0, 0.0, 0.0, 0.0, 0.0, 0.3, 0.0, 0.2, 0.0, 0.0, 0.2, 0.0 },
      { 0.9f, 0.55f, 0.8f, 1.3f }, 0.6f, 0.9f,
      { 0.45, 0.25, 0.20, 0.10 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.28f, 0.6f, 16.0f,
      { 0.45, 0.05, 0.00, 0.00, 0.00, 0.50 }, 0.12f },   // Dorian and Aeolian only
    // Dark / Forest: darker modes, less lead, denser percussion, short intros.
    { "Dark Forest", 149.0, 3.0,
      { 0.20, 0.40, 0.25, 0.10, 0.05, 0.00 },
      { 0.50, 0.30, 0.20 },
      { 0.0, 0.4, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.2f, 0.4f, 0.9f, 0.8f }, 1.2f, 1.1f,
      { 0.25, 0.25, 0.35, 0.15 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.18f, 1.3f, 8.0f,
      { 0.20, 0.40, 0.30, 0.10, 0.00, 0.00 }, 0.35f },
    // Hi-Tech: fastest, busiest, shortest sections.
    { "Hi-Tech", 158.0, 4.0,
      { 0.20, 0.35, 0.25, 0.15, 0.05, 0.00 },
      { 0.55, 0.15, 0.30 },
      { 0.0, 0.3, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.3f, 0.6f, 1.1f, 0.6f }, 1.4f, 1.2f,
      { 0.20, 0.30, 0.35, 0.15 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.16f, 1.4f, 8.0f,
      { 0.20, 0.35, 0.30, 0.15, 0.00, 0.00 }, 0.40f },
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

/**
 * @brief The mode one section borrows over the tonic pedal, or @p trackScale when it borrows nothing.
 *
 * The style profile says which modes are available at all and how often a section borrows; the
 * section's energy says how far it may reach, by multiplying each mode's weight with
 * @c exp(kInterchangeEnergy * (energy - 0.7) * colourTones(mode)). At a drop (energy near 1) that
 * lifts double harmonic by a factor of about 1.6 and Phrygian dominant by 1.4 over Aeolian; in a
 * breakdown (0.3) it presses them down by the same amount, so the Hijaz colour arrives where the
 * literature puts it -- at the peak (Easwaran 2004) -- rather than at random.
 *
 * Everything hangs off the section's own seed, because a section is a lockable unit (PLAN 6.8).
 */
static int borrowedMode(const StyleProfile& s, uint64_t sectionSeed, int trackScale, float energy)
{
    if (s.interchangeChance <= 0.0f) return trackScale;
    Rng r;
    r.seed(mixSeed(sectionSeed ^ kSaltMode, 0));
    if (r.uniform() >= s.interchangeChance) return trackScale;
    double w[kNumScales];
    double total = 0.0;
    for (int m = 0; m < kNumScales; ++m) {
        w[m] = s.interchangeWeight[m] * std::exp(kInterchangeEnergy * (static_cast<double>(energy) - 0.7)
                                                 * scaleColourTones(m));
        if (m == trackScale) w[m] = 0.0;   // "borrowing" the mode the track already has is not a borrow
        total += w[m];
    }
    if (total <= 0.0) return trackScale;
    return drawIndex(r, w, kNumScales);
}

FormPlan makeFormPlan(const StyleProfile& s, uint64_t seed, int target, double arcIn, double arcOut,
                      int trackScale, const uint64_t* sectionSeed)
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
    // The mode of each section, over the tonic pedal (Form.h). It is drawn after the energies,
    // because the energy is what decides how far a section may reach for the Hijaz colour, and from
    // the section's own seed, so that locking or rerolling a section moves its mode with it. The
    // intro and the outro always keep the track's mode: they are the DJ-friendly ends of the track
    // and the next track's intro is written over them (PLAN 6.7).
    trackScale = std::clamp(trackScale, 0, kNumScales - 1);
    f.scaleMask = 1u << trackScale;
    for (int i = 0; i < f.count; ++i) {
        Section& sec = f.section[i];
        const bool ends = sec.type == SectionType::Intro || sec.type == SectionType::Outro;
        const uint64_t ss = sectionSeed != nullptr ? sectionSeed[std::clamp(i, 0, kMaxSections - 1)]
                                                   : mixSeed(seed ^ kSaltMode, static_cast<uint64_t>(i));
        sec.scale = ends ? trackScale
                         : borrowedMode(s, ss, trackScale, 0.5f * (sec.energy + sec.energyTo));
        f.scaleMask |= 1u << std::clamp(sec.scale, 0, kNumScales - 1);
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
    bp.scale = static_cast<int8_t>(s.scale);   // the melodic layer's mode here; the bass ignores it

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

/**
 * @brief The effects of a track: a marker at every section transition, ear candy inside the long ones.
 *
 * **Transitions (18.09.2026: every one of them, not a draw per effect).** Until this round each effect
 * of a transition was drawn with the probability @p amount, and a whole track carried five, nine or one
 * effect -- the user heard "hardly any effects". A transition marker is part of the form, so it is now
 * placed whenever @p amount is at least one half, and below that with the probability 2 * amount, so
 * the knob still thins them out and at zero removes them:
 *  - into a **buildup**: a reverse swell over the bar before it, ending on its first downbeat;
 *  - **buildup to drop**: the riser over the last eight bars, the sweep over the last two, the formant
 *    shot on beat 4 of the pre-drop break and the impact on the drop (unchanged);
 *  - any **other drop** (out of a breakdown, a groove or an intro): a two-bar sweep ending on it and the
 *    impact on its downbeat;
 *  - into a **breakdown**: a one-bar reverse swell ending on its first beat and a four-bar downlifter
 *    from it (the swell is the inhale, the downlifter the fall into the empty floor);
 *  - out of the **intro**: a two-bar sweep into the first section;
 *  - into the **outro**: a four-bar downlifter; the outro's last eight bars keep the sweep that masks
 *    the key change into the next track (PLAN 6.7).
 *
 * **Ear candy.** Inside every section that is not a buildup, at the end of every eight- or
 * sixteen-bar group (Easwaran 2004: something changes every four or eight bars; Butler 2006 on the
 * hypermetre), one short effect with the probability @p amount: a zap on beat 4, a one-bar sweep of
 * filtered noise, a one-bar reverse swell into the next group's downbeat, or a two-bar noise wash.
 * Which of them a track prefers and whether its period is eight or sixteen bars is drawn from the
 * track's seed, so two tracks do not decorate alike. Nothing is ever placed inside a buildup: its last
 * bar is the pre-drop break, and that vacuum is what the drop is heard against.
 *
 * Only the effect types the engine already has are used, and Impact and Riser stay reserved for the
 * drop: the self test checks that they sit nowhere else.
 */
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
    const float pMark = std::min(1.0f, 2.0f * amount);   ///< probability of a transition marker
    auto mark = [&]() { return r.uniform() < pMark; };
    const double bar = kBeatsPerBar;
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        const double end = static_cast<double>(s.startBar + s.bars) * bar;
        const SectionType prev = i > 0 ? f.section[i - 1].type : SectionType::Intro;
        if (s.type == SectionType::Build) {
            // The riser climbs over the last eight bars and arrives on the drop; the formant shot is
            // the pre-drop "Abriss" on beat 4 of the PDB bar; the sweep falls into the drop, which
            // Solberg and Dibben found to be the marker listeners react to.
            const double rise = std::min(8.0, static_cast<double>(s.bars)) * bar;
            if (i > 0 && mark()) add(start - bar, static_cast<float>(bar), SfxType::ReverseSwell);
            if (mark()) add(end - rise, static_cast<float>(rise), SfxType::Riser);
            if (mark()) add(end - 1.0, 0.5f, SfxType::FormantShot);
            if (mark()) add(end - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep);
            if (mark()) add(end, 4.0f, SfxType::Impact);
        } else if (s.type == SectionType::Break) {
            if (i > 0 && mark()) add(start - bar, static_cast<float>(bar), SfxType::ReverseSwell);
            if (mark()) add(start, static_cast<float>(4.0 * bar), SfxType::Downlifter);
        } else if (s.type == SectionType::Drop && i > 0 && prev != SectionType::Build) {
            // A drop that no buildup announced (the flat Progressive body, a groove straight into it).
            if (mark()) add(start - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep);
            if (mark()) add(start, 4.0f, SfxType::Impact);
        } else if (s.type == SectionType::Outro) {
            if (i > 0 && mark()) add(start, static_cast<float>(4.0 * bar), SfxType::Downlifter);
            // The sweep over the last eight bars masks the key change into the next track (PLAN 6.7).
            add(end - 8.0 * bar, static_cast<float>(8.0 * bar), SfxType::Sweep);
        }
        // Out of the intro into whatever comes next, unless that is a buildup or drop, which mark
        // their own entry above.
        if (s.type == SectionType::Intro && i + 1 < f.count && f.section[i + 1].type != SectionType::Build
            && f.section[i + 1].type != SectionType::Drop && mark())
            add(end - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep);
    }

    // Ear candy. The track's palette and period come first, from their own generator, so that the
    // transition draws above do not shift when the palette changes and vice versa.
    Rng c;
    c.seed(mixSeed(seed ^ kSaltSfx, 1));
    const double period = (c.uniform() < 0.55f ? 8.0 : 16.0) * bar;
    // Four kinds, weighted per track: one favourite at 2.5, each of the others 1 or (one time in five)
    // 0 -- a track that never zaps is as legitimate as one that zaps at every phrase.
    double w[4];
    for (double& x : w) x = c.uniform() < 0.8f ? 1.0 : 0.0;
    w[c.below(4)] = 2.5;
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.type == SectionType::Build) continue;
        const double start = static_cast<double>(s.startBar) * bar;
        const double end = static_cast<double>(s.startBar + s.bars) * bar;
        // Interior group boundaries only: the section's own ends belong to the transition markers.
        for (double g = start + period; g < end - 1e-9; g += period) {
            if (c.uniform() >= amount) continue;
            switch (drawIndex(c, w, 4)) {
            case 0: add(g - 1.0, 0.5f, SfxType::Zap); break;                                        // beat 4
            case 1: add(g - bar, static_cast<float>(bar), SfxType::Sweep); break;                   // filtered noise, up and down
            case 2: add(g - bar, static_cast<float>(bar), SfxType::ReverseSwell); break;            // into the downbeat
            default: add(g - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep); break;      // a longer wash
            }
        }
    }
    std::stable_sort(f.sfx.begin(), f.sfx.end(), [](const SfxEvent& a, const SfxEvent& b) { return a.beat < b.beat; });
}

/**
 * @brief The macro automation of a section: the acid's ride and the buildup's send (Form.h).
 *
 * **The acid's ride (18.09.2026: the four stages of the user's rule text).** Every section but a
 * breakdown rides @c acid.cutoff, @c acid.resonance and @c acid.decay through the cycle of RideShape
 * (Form.h): closed, medium resonance and a short decay for the dry click of stage 1; the decay back to
 * the knob and the cutoff opening in stage 2; the resonance up to the squelch in stage 3; fully open
 * in stage 4 and a dive over the last bar. The cycle is 32 bars, or the whole section when that is
 * shorter -- a 16-bar buildup plays it in 16 bars and its dive ends exactly on the drop. The phase
 * before (16.09.2026) rode cutoff and resonance in 8- or 16-bar sawtooth periods with a peak-to-peak
 * of 0.8 octaves; the user heard that as "the acid does not develop". The depth is drawn from the
 * section's seed (0.75 .. 1 of kRideCutoff). Decay and resonance are written as their own strands,
 * relative to the knobs: the decay as ratios of the knob (kRideDecayShort, kRideDecayLong), the
 * resonance to "at most the knob, 0.55" and "at least the knob, 0.85". One consequence to know: these
 * strands replace, for the rest of the section, the track's recipe offset on decay and resonance that
 * Composer writes at the track's start -- a control strand holds one offset, not a sum.
 * A breakdown does what it did: one dive down over the first half and a return over the second.
 *
 * **Why it cannot fight the accent sweep.** The 303's accent charges a capacitor with kSweepTau =
 * 150 ms (Acid.cpp), so the accent's own movement lives inside a sixteenth (103 ms at 145 BPM) and is
 * gone within two. The ride's ramps are two bars and longer (thirty times the capacitor), except the
 * dive, which is one bar -- 1.65 s at 145 BPM, still eleven times the capacitor's time constant. The
 * two are separated in *rate*, not in amount, and `Tools/ref_arrange.py --ride` measures them the same
 * way: the centroid of every sixteenth folded onto its bar, deviation inside a bar against movement
 * between bars.
 *
 * **Why the level match survives.** A wide-open filter is louder, and Composer's loudness probe
 * measures two bars of the track with the *knobs* rather than with the automation, so a ride that
 * added level on average would bias every track's gain. The cycle's cutoff shape is therefore centred
 * on the section's line: its mean over the cycle, computed from the key points with raised-cosine
 * ramps (a raised cosine from a to b averages (a + b) / 2), is subtracted (RideShape::meanRaw), and
 * the self test measures what is left.
 *
 * **The buildup's send.** The third strand of the review's snare roll: over the last four bars of a
 * buildup the percussion's hall send climbs to kRollSend and is cut back to nothing at the section
 * that follows, which is the drop. Nothing else is needed for the cut -- the pre-drop break already
 * empties beat 4 (Form.h) -- and a send that is still open into the drop would smear exactly the
 * transient the whole gesture exists to sharpen.
 */
void sectionAutomation(const ParamStore& p, const Section& s, uint64_t seed, double beat,
                       float base0, float base1, bool knobs, std::vector<ControlEvent>& out)
{
    const int ab = p.base(Module::Acid), mb = p.base(Module::Mix);
    // The strands below are written one after the other; the events are put into time order at the
    // end (stable, so two events of one parameter at the same beat keep their order), which is the
    // order every consumer of a control stream expects.
    const size_t first = out.size();
    struct Sorter {
        std::vector<ControlEvent>& v;
        size_t from;
        ~Sorter() { std::stable_sort(v.begin() + static_cast<std::ptrdiff_t>(from), v.end(),
                                     [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; }); }
    } sorter{ out, first };
    auto push = [&](int id, float value, double at, float len) {
        ControlEvent c;
        c.beat = at;
        c.param = static_cast<int16_t>(id);
        c.kind = ControlEvent::Kind::Offset;
        c.value = value;
        c.length = len;
        out.push_back(c);
    };
    // The percussion's hall send: down at the start of every section, up over the last four bars of a
    // buildup. Writing the zero unconditionally is what makes the cut at the drop exact -- the drop is
    // a section like any other and its first event closes the send.
    push(mb + mix::PercHall, 0.0f, beat, 0.0f);
    const double bars = static_cast<double>(s.bars);
    if (s.type == SectionType::Build && !knobs) {
        const double rollBars = std::min(static_cast<double>(kRollBars), bars);
        push(mb + mix::PercHall, kRollSend, beat + (bars - rollBars) * kBeatsPerBar,
             static_cast<float>(rollBars * kBeatsPerBar));
    }
    if (knobs) return;

    // The acid's ride. Sections shorter than four bars have nothing to ride on.
    if (bars < 4.0) return;
    Rng r;
    r.seed(mixSeed(seed ^ kSaltRide, 0));
    // Where the straight line of the section's own arc stands at bar b, so the ride is an excursion
    // from it and not a replacement for it.
    auto base = [&](double b) { return base0 + (base1 - base0) * static_cast<float>(b / bars); };
    const float depth = 0.75f + 0.25f * r.uniform();   ///< how hard this section is ridden
    if (s.type == SectionType::Break) {
        // The dive: down over the first half, back over the second. A breakdown is where a psytrance
        // track closes the filter, and the return is what makes the following section open.
        const double half = bars / 2.0;
        const float len = static_cast<float>(half * kBeatsPerBar);
        push(ab + acid::Cutoff, base(half) - depth * kRideDive, beat, len);
        push(ab + acid::Resonance, -depth * kRideReso, beat, len);
        push(ab + acid::Cutoff, base1, beat + half * kBeatsPerBar, len);
        push(ab + acid::Resonance, 0.0f, beat + half * kBeatsPerBar, len);
        return;
    }
    // Everything else rides the four-stage cycle (Form.h, RideShape): 32 bars, or the whole section
    // when it is shorter, so that a buildup's cycle ends in the dive exactly on the drop.
    const RideShape shape = acidRideShape(bars);
    const double knobReso = p.get(ab + acid::Resonance);
    const double knobDecay = p.get(ab + acid::Decay);
    const ParamDesc& dd = p.desc(ab + acid::Decay);
    // Decay is a log parameter: a ratio of the knob is a fixed normalised distance.
    const double decaySpan = std::log(static_cast<double>(dd.maxValue) / dd.minValue);
    auto decayOffset = [&](double ratio) {
        const double target = std::clamp(knobDecay * ratio, static_cast<double>(dd.minValue), static_cast<double>(dd.maxValue));
        return static_cast<float>(std::log(target / knobDecay) / decaySpan);
    };
    const float resoMedium = static_cast<float>(std::min(knobReso, static_cast<double>(kRideResoMedium)) - knobReso);
    const float resoSquelch = static_cast<float>(std::max(knobReso, static_cast<double>(kRideResoSquelch)) - knobReso);
    const float h = 0.5f * depth * kRideCutoff;   ///< half the peak-to-peak cutoff excursion
    for (double c0 = 0.0; c0 + shape.length <= bars + 1e-9; c0 += shape.length) {
        const double at0 = beat + c0 * kBeatsPerBar;
        auto at = [&](double barInCycle) { return at0 + barInCycle * kBeatsPerBar; };
        auto len = [&](double barsLong) { return static_cast<float>(barsLong * kBeatsPerBar); };
        auto cut = [&](int k) { return base(c0 + shape.bar[k]) + h * shape.cutoff[k]; };
        // Cutoff: into "almost closed" over the first bar, hold, open over stages 2 to 4, dive.
        push(ab + acid::Cutoff, cut(0), at(0.0), len(1.0));
        for (int k = 1; k < RideShape::kPoints; ++k)
            push(ab + acid::Cutoff, cut(k), at(shape.bar[k - 1] + shape.hold[k - 1]),
                 len(shape.bar[k] - shape.bar[k - 1] - shape.hold[k - 1]));
        // Resonance: medium through stages 1 and 2, up to the squelch over stage 3, held to the end.
        push(ab + acid::Resonance, resoMedium, at(0.0), len(1.0));
        push(ab + acid::Resonance, resoSquelch, at(shape.stage[2]), len(shape.stage[3] - shape.stage[2]));
        // Decay: short and dry in stage 1, back to the knob over stage 2, longer through 3 and 4.
        push(ab + acid::Decay, decayOffset(kRideDecayShort), at(0.0), len(1.0));
        push(ab + acid::Decay, 0.0f, at(shape.stage[1]), len(shape.stage[2] - shape.stage[1]));
        push(ab + acid::Decay, decayOffset(kRideDecayLong), at(shape.stage[2]), len(shape.stage[3] + (shape.length - shape.stage[3]) * 0.5 - shape.stage[2]));
    }
}

RideShape acidRideShape(double sectionBars)
{
    RideShape s;
    s.length = sectionBars >= 32.0 ? 32.0 : sectionBars;
    const double L = s.length;
    for (int k = 0; k < 4; ++k) s.stage[k] = L * k / 4.0;
    // The dive takes the last bar (the last two of a 32-bar cycle would eat a quarter of stage 4);
    // the full opening is reached halfway through stage 4 and held until the dive.
    const double dive = 1.0;
    // Key points of the cutoff, in units of half the excursion before centring, and when each is
    // reached: closed through stage 1, a little open at the end of stage 2, well open at the end of
    // stage 3, fully open halfway into stage 4, closed again at the end of the dive.
    const double raw[RideShape::kPoints] = { -1.0, -1.0, -0.2, 0.5, 1.0, 1.0, -1.0 };
    const double when[RideShape::kPoints] = { 0.0, L / 4.0, L / 2.0, 3.0 * L / 4.0, 7.0 * L / 8.0, L - dive, L };
    // The cycle's mean, with raised-cosine ramps between the points (a raised cosine between a and b
    // averages to (a + b) / 2) -- subtracted, so the ride swings around the section's own line and
    // the level match, which measures the knobs, stays honest (Form.h).
    double area = 0.0;
    for (int k = 1; k < RideShape::kPoints; ++k) area += 0.5 * (raw[k] + raw[k - 1]) * (when[k] - when[k - 1]);
    const double mean = area / L;
    for (int k = 0; k < RideShape::kPoints; ++k) {
        s.cutoff[k] = static_cast<float>(raw[k] - mean);
        s.bar[k] = when[k];
        s.hold[k] = 0.0;
    }
    // Point 0 is reached over the first bar (from wherever the previous section left the filter) and
    // held to point 1; the events of point k start where point k-1 was reached plus its hold.
    s.hold[0] = 1.0;
    s.meanRaw = static_cast<float>(mean);
    return s;
}

} // namespace phos
