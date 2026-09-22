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
constexpr uint64_t kSaltFuzz    = 0x46555A5A0000000Dull;   ///< the form's fuzziness (23.09.2026, round "Form")
constexpr uint64_t kSaltMode    = 0x4D4F44450000005ull;   ///< the section's borrowed mode (16.09.2026)
constexpr uint64_t kSaltRide    = 0x5249444500000006ull;   ///< the section's macro ride (16.09.2026)
constexpr uint64_t kSaltPsy     = 0x5053594300000007ull;   ///< the psychedelic ear candy (19.09.2026)
constexpr uint64_t kSaltVoice   = 0x564F494300000008ull;   ///< the voices' placement (19.09.2026)
constexpr uint64_t kSaltBed     = 0x4245440000000009ull;   ///< the shamanic bed's placement (19.09.2026)
constexpr uint64_t kSaltFxRide  = 0x46585244000000Aull;    ///< the modulation effects' section ride (19.09.2026)
constexpr uint64_t kSaltVariant = 0x564152490000000Bull;   ///< the voices' and the bed's variants (19.09.2026, round "voices")
constexpr uint64_t kSaltClimax  = 0x434C494D0000000Cull;   ///< drop 2's squelches (19.09.2026, round "arrangement")

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
//
// 19.09.2026, round "arrangement": the user's subgenre rules stand above the measured values. The tempo
// windows are now the rule's -- Full-On 142 .. 146, Progressive 135 .. 138, Goa 142 .. 148, Dark/Forest
// 148 .. 155 (Hi-Tech has no rule and keeps 154 .. 162) -- the measured Goa median of 142.8 lies inside its
// window, the measured Full-On median of 144.6 inside its. The body weights pick each style's own template
// (Form.cpp, kTemplates) with certainty: the rule text gives every subgenre one form. The fourth PDB variant
// (the kick alone on beat 4) is never drawn any more: "on beat 4 of the last bar before the drop everything
// stops". breakShare and introBars are no longer read (the templates fix both).
const StyleProfile kProfiles[kNumStyles] = {
    // Goa
    { "Goa", 145.0, 3.0,
      { 0.15, 0.20, 0.15, 0.30, 0.15, 0.05 },          // scales: Phrygian dominant and double harmonic lead
      { 0.0, 0.0, 1.0, 0.0 },                          // template: Goa
      { 0.0, 0.5, 0.0, 0.0, 0.0, 0.2, 0.0, 0.1, 0.0, 0.0, 0.4, 0.0 },   // i-bII and i-bVII
      { 1.15f, 1.10f, 1.10f, 1.25f, 0.80f, 1.20f, 1.20f }, 0.9f, 0.95f,
      { 0.20, 0.25, 0.55, 0.00 },                      // mostly beat 4 alone: rarely total silence
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.26f, 1.0f, 16.0f,
      { 0.15, 0.25, 0.10, 0.30, 0.15, 0.05 }, 0.45f, StyleId::Goa,
      // lead vector (22.09.2026): dense sixteenths, wide, more leaps, little portamento; surge and arch
      { 0.80f, 0.80f, 0.80f, 0.60f, 1.10f, 0.15f, 0.35f, 0.30f, 0.05f,
        { 0.35, 0.25, 0.10, 0.20, 0.10 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.25, 0.25, 0.15, 0.15, 0.10, 0.10 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        7, { 0.5, 0.3, 0.0, 0.2 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Goa: G4 .. F#5, echo and polyphony   // interchange: the Hijaz modes at the peak
    // Full-On: the default, every multiplier 1, so the knobs play as they are set.
    { "Full-On", 144.0, 2.0,
      { 0.30, 0.25, 0.15, 0.15, 0.05, 0.10 },
      { 1.0, 0.0, 0.0, 0.0 },                          // template: the strict two-drop form
      { 0.0, 0.1, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.1, 0.0 },
      { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f, 1.0f,
      { 0.40, 0.30, 0.30, 0.00 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.22f, 1.0f, 16.0f,
      { 0.30, 0.30, 0.15, 0.15, 0.05, 0.05 }, 0.30f, StyleId::FullOn,
      // lead vector (22.09.2026): call-and-response: medium density, smooth, sliding, tied to the bass
      { 0.60f, 1.00f, 1.00f, 1.00f, 1.00f, 0.35f, 0.40f, 0.60f, 0.06f,
        { 0.30, 0.30, 0.15, 0.10, 0.15 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.30, 0.20, 0.10, 0.15, 0.15, 0.10 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        5, { 0.4, 0.5, 0.1, 0.0 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Full-On: F4 .. E5, call and response
    // Progressive: flatter form, fewer leads, more pad, Dorian and Aeolian.
    { "Progressive", 136.5, 1.5,
      { 0.35, 0.10, 0.05, 0.05, 0.00, 0.45 },
      { 0.0, 1.0, 0.0, 0.0 },                          // template: long cycles, dry drops
      { 0.0, 0.0, 0.0, 0.0, 0.0, 0.3, 0.0, 0.2, 0.0, 0.0, 0.2, 0.0 },
      { 0.9f, 0.55f, 0.50f, 0.8f, 1.20f, 1.3f, 1.30f }, 0.6f, 0.9f,
      { 0.50, 0.30, 0.20, 0.00 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.28f, 0.6f, 16.0f,
      { 0.45, 0.05, 0.00, 0.00, 0.00, 0.50 }, 0.12f, StyleId::Progressive,
      // lead vector (22.09.2026): sparse, homing, stable, low entropy: the pedal and thinning
      { 0.30f, 1.30f, 1.30f, 1.30f, 0.70f, 0.45f, 0.20f, 0.70f, 0.04f,
        { 0.10, 0.25, 0.45, 0.15, 0.05 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.25, 0.10, 0.10, 0.05, 0.20, 0.30 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        4, { 0.3, 0.1, 0.6, 0.0 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Progressive: E4 .. D#5, timbral   // Dorian and Aeolian only
    // Dark / Forest: darker modes, less lead, denser percussion.
    { "Dark Forest", 151.5, 3.5,
      { 0.20, 0.40, 0.25, 0.10, 0.05, 0.00 },
      { 0.0, 0.0, 0.0, 1.0 },                          // template: fewer conventional drops
      { 0.0, 0.4, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.2f, 0.4f, 0.50f, 0.9f, 1.00f, 0.8f, 1.30f }, 1.2f, 1.1f,
      { 0.30, 0.30, 0.40, 0.00 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.18f, 1.3f, 8.0f,
      { 0.20, 0.40, 0.30, 0.10, 0.00, 0.00 }, 0.35f, StyleId::DarkForest,
      // lead vector (22.09.2026): tension tones allowed, syncopation, the unresolved call
      { 0.50f, 1.10f, 0.70f, 0.90f, 1.10f, 0.40f, 0.35f, 0.50f, 0.05f,
        { 0.15, 0.10, 0.25, 0.15, 0.35 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.20, 0.10, 0.10, 0.15, 0.25, 0.20 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        4, { 0.2, 0.0, 0.5, 0.3 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Dark Forest: E4 .. D#5, timbral and hocket
    // Hi-Tech: fastest, busiest; no rule of its own, so the strict two-drop form.
    { "Hi-Tech", 158.0, 4.0,
      { 0.20, 0.35, 0.25, 0.15, 0.05, 0.00 },
      { 1.0, 0.0, 0.0, 0.0 },
      { 0.0, 0.3, 0.0, 0.0, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.3, 0.0 },
      { 1.3f, 0.6f, 0.80f, 1.1f, 1.20f, 0.6f, 0.50f }, 1.4f, 1.2f,
      { 0.25, 0.35, 0.40, 0.00 },
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.16f, 1.4f, 8.0f,
      { 0.20, 0.35, 0.30, 0.15, 0.00, 0.00 }, 0.40f, StyleId::HiTech,
      // lead vector (22.09.2026): densest, freest, hocketing against the bass, hardly a slide
      { 0.90f, 0.70f, 0.60f, 0.40f, 1.30f, 0.10f, 0.50f, 0.80f, 0.07f,
        { 0.25, 0.10, 0.15, 0.20, 0.30 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.05, 0.20, 0.20, 0.15, 0.30, 0.10 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        7, { 0.3, 0.1, 0.0, 0.6 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Hi-Tech: G4 .. F#5, hocket
};

/** @brief Control points of the arcs: (t, E) pairs, interpolated with raised cosines. */
struct ArcPoint { double t, e; };
const ArcPoint kWarmUp[]   = { { 0.0, 0.25 }, { 0.35, 0.45 }, { 0.75, 0.70 }, { 1.0, 0.85 } };
const ArcPoint kPeakTime[] = { { 0.0, 0.70 }, { 0.30, 0.95 }, { 0.65, 1.00 }, { 1.0, 0.85 } };
const ArcPoint kMorning[]  = { { 0.0, 0.80 }, { 0.30, 0.70 }, { 0.70, 0.55 }, { 1.0, 0.45 } };
const ArcPoint kClosing[]  = { { 0.0, 0.90 }, { 0.25, 0.75 }, { 0.70, 0.45 }, { 1.0, 0.20 } };
const ArcPoint kFlat[]     = { { 0.0, 0.70 }, { 1.0, 0.70 } };

bool isCore(SectionType t) { return t == SectionType::Groove || t == SectionType::Drop; }

/**
 * @brief The two-drop templates (19.09.2026, round "arrangement"): the user's macro form, one per
 *        subgenre family.
 *
 * Every template has the same eight slots -- intro, groove, buildup 1, drop 1, breakdown, the big
 * buildup, drop 2, outro -- so that everything downstream can name them (kSlot*). The lengths are the
 * rule text's at 256 bars:
 *
 * | slot        | Full-On / Hi-Tech | Progressive | Goa     | Dark Forest |
 * |-------------|-------------------|-------------|---------|-------------|
 * | intro       | 1-32              | 1-32        | 1-32    | 1-32        |
 * | groove      | 33-64             | 33-96 (64)  | 33-64   | 33-64       |
 * | buildup 1   | 65-80, PDB 77-80  | 97-112      | 65-80   | 65-80, no PDB |
 * | drop 1      | 81-112            | 113-144     | 81-112  | 81-128 (48) |
 * | breakdown   | 113-144           | 145-176     | 113-144, no cut, arp spiral | 129-144 (16), percussion stays |
 * | big buildup | 145-176, roll 161-176 | 177-192 (16), roll 8 | 145-176 | 145-176 |
 * | drop 2      | 177-224 (48)      | 193-224 (32), dry | 177-224 | 177-224 |
 * | outro       | 225-256           | 225-256     | 225-256 | 225-256     |
 *
 * Progressive's longer cycles are its 64-bar groove and its short big buildup; its drops are dry (no
 * impact, the stab carries them). Goa's breakdown is never silent -- the arp spirals through it, and with
 * the big buildup behind it that is the rule's 64 bars of building spirals. Dark Forest has "fewer
 * conventional drops": its first buildup runs into drop 1 without a pre-drop break, and its short
 * breakdown keeps two layers of percussion; drop 2 keeps the full gesture, because drop 2 is the climax
 * in every style.
 */
enum : int { kSlotIntro = 0, kSlotGroove, kSlotBuild1, kSlotDrop1, kSlotBreak, kSlotBuild2, kSlotDrop2, kSlotOutro, kSlots };
struct Template {
    int bars[kSlots];      ///< length of each slot at 256 bars
    int grow[8];           ///< the slots that take 16 more bars when the track is longer, in order (-1 ends)
    int growCap[8];        ///< the length each of those grows to at most
};
const SectionType kSlotType[kSlots] = { SectionType::Intro, SectionType::Groove, SectionType::Build, SectionType::Drop,
                                        SectionType::Break, SectionType::Build, SectionType::Drop, SectionType::Outro };
const Template kTemplates[kNumBodies] = {
    // Full-On (and Hi-Tech): the strict two-drop form.
    { { 32, 32, 16, 32, 32, 32, 48, 32 }, { kSlotDrop2, kSlotDrop1, kSlotBreak, kSlotGroove, kSlotDrop1, kSlotBreak, kSlotGroove, -1 },
      { 64, 48, 48, 48, 64, 64, 64, 0 } },
    // Progressive: the 64-bar groove, a short big buildup, a 32-bar drop 2.
    { { 32, 64, 16, 32, 32, 16, 32, 32 }, { kSlotDrop2, kSlotDrop1, kSlotBreak, kSlotDrop2, kSlotDrop1, kSlotBreak, -1, -1 },
      { 48, 48, 48, 64, 64, 64, 0, 0 } },
    // Goa: the Full-On lengths, with the spiral in the breakdown.
    { { 32, 32, 16, 32, 32, 32, 48, 32 }, { kSlotDrop2, kSlotBreak, kSlotDrop1, kSlotGroove, kSlotBreak, kSlotDrop1, kSlotGroove, -1 },
      { 64, 48, 48, 48, 64, 64, 64, 0 } },
    // Dark Forest: the long drop 1, the short breakdown.
    { { 32, 32, 16, 48, 16, 32, 48, 32 }, { kSlotDrop2, kSlotDrop1, kSlotGroove, kSlotBreak, kSlotGroove, kSlotBreak, -1, -1 },
      { 64, 64, 48, 32, 64, 48, 0, 0 } },
};
/** @brief The order in which slots give up 16 bars when the track is shorter, and how far (all templates). */
const int kShrink[9][2] = { { kSlotDrop2, 32 }, { kSlotGroove, 16 }, { kSlotBreak, 16 }, { kSlotDrop1, 16 },
                            { kSlotBuild2, 16 }, { kSlotIntro, 16 }, { kSlotOutro, 16 }, { kSlotDrop2, 16 }, { kSlotGroove, 16 } };

/** @brief True while every hard constraint holds (exposed as formConstraintsHold; Form.h says which). */
bool constraintsHoldImpl(const FormPlan& f)
{
    int climaxes = 0, lastDrop = -1;
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.bars < 8 || s.bars % 8 != 0 || s.startBar % 8 != 0) return false;
        if (isCore(s.type) && s.bars < 16) return false;
        if (s.type == SectionType::Build && (s.bars < 8 || s.bars > 32)) return false;
        if (s.type == SectionType::Break && s.bars < 16) return false;
        if (s.type == SectionType::Drop) lastDrop = i;
        if (s.climax) ++climaxes;
    }
    if (f.count < 2 || f.section[0].type != SectionType::Intro || f.section[0].bars < kDjOverlap) return false;
    if (f.section[f.count - 1].type != SectionType::Outro || f.section[f.count - 1].bars < kDjOverlap) return false;
    return climaxes == 1 && lastDrop >= 0 && f.section[lastDrop].climax;
}

/** @brief Recomputes the start bars and the total after a length changed. */
void relayout(FormPlan& f)
{
    int at = 0;
    for (int i = 0; i < f.count; ++i) { f.section[i].startBar = at; at += f.section[i].bars; }
    f.bars = at;
}

/**
 * @brief The slot lengths of template @p t for a track of @p target bars (a multiple of 16).
 *
 * From the rule's 256 bars, 16 bars at a time: a longer track lengthens the drops first, then the
 * breakdown and the groove (each up to its cap); a shorter one gives up drop 2's extra half, then the
 * groove, the breakdown, drop 1, the big buildup, and last the intro and the outro (never under the DJ
 * overlap). The walk is deterministic, so a length always maps to the same form.
 */
void fitTemplate(const Template& t, int target, int* bars)
{
    int total = 0;
    for (int k = 0; k < kSlots; ++k) { bars[k] = t.bars[k]; total += bars[k]; }
    for (bool moved = true; total < target && moved;) {
        moved = false;
        for (int g = 0; g < 8 && t.grow[g] >= 0 && total < target; ++g)
            if (bars[t.grow[g]] + 16 <= t.growCap[g]) { bars[t.grow[g]] += 16; total += 16; moved = true; }
    }
    for (int k = 0; k < 9; ++k) {
        const int slot = kShrink[k][0], floor = kShrink[k][1];
        while (total > target && bars[slot] - 16 >= floor) { bars[slot] -= 16; total -= 16; }
    }
}

/**
 * @brief The form's fuzziness (23.09.2026, round "Form"): one or two eight-bar moves between neighbouring
 *        sections, drawn from the form's seed.
 *
 * The user: "Die Vorgabe der Takte scheinst du SEHR ernst genommen zu haben, dadurch haben alle Stuecke eines
 * Genres immer genau dieselbe Einteilung. Da sollten wir durchaus eine (minimale) Fuzziness erlauben." The
 * template stays the rule and the total stays exact; a track moves eight bars from one section to its
 * neighbour once (seven in ten tracks) or twice, wherever both stay inside their bounds -- an intro of 16
 * to 48, a groove of 16 to 96, a buildup of 8 to 32, a drop of 24 to 64, a breakdown of 16 to 64, an outro
 * of 16 to 48 (the literature's frames: intro one to two minutes, breakdown 64 to 96 bars with an arc, drop
 * 32 to 64; Psytrance Blueprint, "7 rules of structure"; Psychedelic Island, "Track structure"). Everything
 * downstream reads the sections it gets: the roll and the pre-drop break of a buildup are cut from its
 * length, the energies from the arc at the bars the sections really have.
 */
void jitterForm(int* bars, uint64_t seed)
{
    static const int kMin[kSlots] = { kDjOverlap, 16, 8, 24, 16, 8, 24, kDjOverlap };
    static const int kMax[kSlots] = { 48, 96, 32, 64, 64, 32, 64, 48 };
    static const int kPairs[7][2] = { { kSlotIntro, kSlotGroove }, { kSlotGroove, kSlotBuild1 }, { kSlotBuild1, kSlotDrop1 }, { kSlotDrop1, kSlotBreak },
                                      { kSlotBreak, kSlotBuild2 }, { kSlotBuild2, kSlotDrop2 }, { kSlotDrop2, kSlotOutro } };
    Rng j;
    j.seed(mixSeed(seed ^ kSaltFuzz, 0));
    const int moves = j.uniform() < 0.3f ? 2 : 1;
    for (int m = 0; m < moves; ++m) {
        const int pair = j.below(7), dir = j.below(2);
        const int from = kPairs[pair][dir], to = kPairs[pair][1 - dir];
        if (bars[from] - 8 >= kMin[from] && bars[to] + 8 <= kMax[to]) { bars[from] -= 8; bars[to] += 8; }
    }
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
    target = std::clamp((target / kTrackBarStep) * kTrackBarStep, kMinTrackBars, kMaxTrackBars);

    // The template is the style's (19.09.2026: the rule text gives every subgenre one form; the draw
    // stays a draw so that a profile may one day mix them).
    f.body = drawIndex(r, s.bodyWeight, kNumBodies);
    const Template& tp = kTemplates[f.body];
    int lens[kSlots];
    fitTemplate(tp, target, lens);
    jitterForm(lens, seed);   // the form's fuzziness (23.09.2026)
    f.count = kSlots;
    for (int k = 0; k < kSlots; ++k) {
        f.section[k].type = kSlotType[k];
        f.section[k].bars = lens[k];
    }
    relayout(f);

    // The form's own decisions per slot (Form.h, Section): the rolls, the pre-drop breaks, what the
    // styles do differently.
    Section& build1 = f.section[kSlotBuild1];
    Section& build2 = f.section[kSlotBuild2];
    Section& brk = f.section[kSlotBreak];
    f.section[kSlotDrop2].climax = true;
    // Buildup 1: a four-bar roll inside the four-bar pre-drop break (bars 77-80 of the rule), except in
    // Dark Forest, whose first drop comes without the conventional stop.
    build1.rollBars = std::min(4, build1.bars);
    build1.pdbBars = f.body == 3 ? 0 : std::min(4, build1.bars);
    // The big buildup: the sixteen-bar roll (quarters, eighths, sixteenths, thirty-seconds, four bars
    // each) over its second half, and the stop on beat 4 of its last bar. A short big buildup (16 bars,
    // Progressive) rolls over its last eight.
    build2.rollBars = std::min(16, build2.bars / 2);
    build2.pdbBars = 1;
    if (f.body == 1) { f.section[kSlotDrop1].dry = true; f.section[kSlotDrop2].dry = true; }
    if (f.body == 2) brk.spiral = true;
    if (f.body == 3) brk.breakPerc = 2;

    // Energies from the arc, and the per-section decisions that belong to the form.
    Rng d;
    d.seed(mixSeed(seed ^ kSaltSection, 0));
    auto scaled = [](float nominal, double arc) { return static_cast<float>(nominal * (0.55 + 0.45 * arc)); };
    for (int i = 0; i < f.count; ++i) {
        Section& sec = f.section[i];
        const double t0 = f.bars > 0 ? static_cast<double>(sec.startBar) / f.bars : 0.0;
        const double t1 = f.bars > 0 ? static_cast<double>(sec.startBar + sec.bars) / f.bars : 1.0;
        const double a0 = arcIn + (arcOut - arcIn) * t0, a1 = arcIn + (arcOut - arcIn) * t1;
        // The arc scales the type's nominal energy; at arc 0 a section keeps 55 % of it, so the order
        // drop > buildup > groove > breakdown survives every arc. Drop 1 stands at kDrop1Share of a drop:
        // the rule makes drop 2 the climax, not a repeat.
        const float nominal = typeEnergy(sec.type) * (sec.type == SectionType::Drop && !sec.climax ? kDrop1Share : 1.0f);
        sec.energy = scaled(nominal, a0);
        sec.energyTo = scaled(nominal, a1);
        if (sec.type == SectionType::Drop) sec.energyTo = sec.energy;   // a drop holds its level
        if (sec.type == SectionType::Build) sec.pdbVariant = drawIndex(d, s.pdbWeight, kNumPdbVariants);
        // The cut of Grosz et al. (1 to 3 s): the bass vacuum of the rule, "kick and bass cut at once".
        // Goa's breakdown is never silent, Dark Forest's keeps its percussion; neither takes a cut.
        if (sec.type == SectionType::Break && i > 0 && isCore(f.section[i - 1].type)) {
            const float cut = d.uniform() < 0.5f ? 1.0f : 2.0f;
            sec.cutBeats = (sec.spiral || sec.breakPerc > 0) ? 0.0f : cut;
        }
    }
    // Drop 2 is the climax: its energy stands kClimaxMargin above every other section's, whatever the
    // arc does across the track (a Closing arc falls towards the end and would otherwise put drop 1 on
    // top). Where the margin would take it past 1, the others come down instead.
    {
        Section& peak = f.section[kSlotDrop2];
        float others = 0.0f;
        for (int i = 0; i < f.count; ++i) if (i != kSlotDrop2) others = std::max({ others, f.section[i].energy, f.section[i].energyTo });
        peak.energy = peak.energyTo = std::min(1.0f, std::max(peak.energy, others + kClimaxMargin));
        for (int i = 0; i < f.count; ++i) {
            if (i == kSlotDrop2) continue;
            f.section[i].energy = std::min(f.section[i].energy, peak.energy - kClimaxMargin);
            f.section[i].energyTo = std::min(f.section[i].energyTo, peak.energy - kClimaxMargin);
        }
    }
    // A buildup starts where the section before it ended and arrives at the drop's energy (just under it:
    // the drop itself is the arrival), so that everything the energy drives -- the gain, the filter arcs --
    // really rises through it (Solberg and Dibben 2019: the rising middle of the U). Goa's breakdown rises
    // into the big buildup as well: the rule's 64 bars of building spirals.
    for (int i = 0; i < f.count; ++i) {
        Section& sec = f.section[i];
        if (sec.type == SectionType::Break && sec.spiral && i + 1 < f.count) sec.energyTo = std::max(sec.energy, 0.5f * (sec.energy + f.section[i + 1].energy + 0.2f));
        if (sec.type != SectionType::Build) continue;
        sec.energy = i > 0 ? f.section[i - 1].energyTo : 0.5f;
        sec.energyTo = i + 1 < f.count ? f.section[i + 1].energy - (f.section[i + 1].climax ? kClimaxMargin : 0.0f) : 1.0f;
        sec.energyTo = std::max(sec.energy, sec.energyTo);
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

float sectionTrimDb(const FormPlan& f, int index, double u)
{
    if (index < 0 || index >= f.count) return 0.0f;
    const Section& s = f.section[index];
    if (s.type == SectionType::Drop && !s.climax) return -kDrop1HoldDb;
    if (s.type == SectionType::Intro) return -kIntroTrimDb;
    if (s.type == SectionType::Build && index + 1 < f.count && f.section[index + 1].climax)
        return -kBuildHeadroomDb * static_cast<float>(std::clamp(u, 0.0, 1.0));
    return 0.0f;
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
    // Drawn and no longer read (19.09.2026: the kick enters on bar 17 of every intro, kIntroKickBar), but
    // still drawn, so that every later draw of the section keeps its place in the stream.
    (void)rs.below(5);
    // A drop brings everything back at once (the instrumentation matrix of PLAN 6.1), which is also
    // what makes Solberg and Dibben's Track 2 rule hold: after the drop the spectrum must be at least
    // as full as it was before the break. Every other section may leave a voice out.
    const bool drop = s.type == SectionType::Drop;
    // Every line's share of the sections is its knob (the lead since 21.09.2026, acid and arp since
    // 22.09.2026). Acid and arp kept a fixed draw here only because they were still thinned once per
    // *track*, and a second thinning would have emptied them twice; that track-level draw is gone
    // (Melody.cpp, makeMelodyPlan), so the thinning happens here alone. The share is about what it
    // was -- acid 0.9 x 0.6 = 0.54 of the sections before, 0.6 now; arp 0.8 x 0.5 = 0.40 against
    // 0.5 -- but it is a share of the sections instead of of the whole night, so a track no longer
    // simply lacks its acid line.
    const bool drawAcid = rs.uniform() < a.amount[mpIndex(MelodyPart::Acid)],
               drawLead = rs.uniform() < a.amount[mpIndex(MelodyPart::Lead)],
               drawArp = rs.uniform() < a.amount[mpIndex(MelodyPart::Arp)];
    const bool useAcid = a.part[mpIndex(MelodyPart::Acid)] && (drop || drawAcid);
    const bool useLead = a.part[mpIndex(MelodyPart::Lead)] && (drop || drawLead);
    const bool useArp = a.part[mpIndex(MelodyPart::Arp)] && (drop || drawArp);
    const bool breakLead = rs.uniform() < 0.5f;     // a breakdown keeps the lead or the arp, not both
    const bool padExtra = rs.uniform() < 0.45f;     // pads join a section that already has lead or acid
    const bool gate = rs.uniform() < 0.35f;
    (void)rs.below(2);                              // the outro's old layer step (19.09.2026: always eight bars)
    // 19.09.2026, round "voices": the draws of the three new voices come after every older draw of the
    // section, so no section decides anything else differently because of them.
    //  - the counter-lead answers the lead; since the arrangement round it is the rule's "lead 2" and
    //    plays in drop 2 only, in call and response with the lead;
    //  - the stab is a surprise, so a section carries it less often, and only every other group of it --
    //    except Progressive's dry drops, which it carries (the rule's "bass stabs");
    //  - the drone lies under every bar of a track that has one (20.09.2026); the coin flip below is the
    //    one the voices round used to place it, kept only so that the draws after it do not move.
    // 20.09.2026, round "dialogue": the user's Model 3 gives the counter-lead the main breakdown as well
    // as drop 2, so the drop-2 exception now covers the breakdown too. The draw stays where it was and
    // is still made, so nothing else this section decides moves.
    const bool useCounter = a.part[mpIndex(MelodyPart::Counter)] && (s.climax || s.type == SectionType::Break || rs.uniform() < 0.75f);
    const bool useStab = a.part[mpIndex(MelodyPart::Stab)] && (rs.uniform() < (drop ? 0.75f : 0.55f) || s.dry);
    const int voiceParity = rs.below(2);            // which eight-bar groups the counter takes; the stab takes the others
    const bool droneCore = a.part[mpIndex(MelodyPart::Drone)] && rs.uniform() < 0.5f;
    const uint8_t bAcid = partBit(MelodyPart::Acid), bLead = partBit(MelodyPart::Lead), bCounter = partBit(MelodyPart::Counter),
                  bArp = partBit(MelodyPart::Arp), bStab = partBit(MelodyPart::Stab), bPad = partBit(MelodyPart::Pad),
                  bDrone = partBit(MelodyPart::Drone);
    const bool hasPad = a.part[mpIndex(MelodyPart::Pad)], hasDrone = a.part[mpIndex(MelodyPart::Drone)];

    // Group-level change: inside a section no eight-bar group repeats its predecessor's change
    // (Easwaran 2004: something new every four or eight bars; Butler's hypermetre).
    const int groupInSection = b / 8;
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

    const int maxLayers = std::max(1, a.percLayers);
    const int hatLayers = std::clamp(a.hatLayers, 1, maxLayers);
    uint8_t parts = 0;
    int layers = maxLayers;
    // A bar that plays nothing but what the rule names: the outro's kick, bass and hat, the last bar of a
    // pre-drop break. No pad, no drone, no voice is added to it below.
    bool bare = false;
    bp.cycleBar = b % 32;

    // 19.09.2026, round "arrangement": the instrumentation of the user's two-drop form. Bars are 0-based
    // here; the comments give the rule's 1-based bars of a 256-bar Full-On track.
    switch (s.type) {
    case SectionType::Intro: {
        // Bars 1-16 without a kick: pads and textures, a quiet closed hat on the sixteenths, the shaker
        // from bar 9 (one change inside the first sixteen). Bars 17-32: kick and bass, the off-beat hat.
        // Inside a set (23.09.2026, round "DJ") the kick enters at the hand-over instead -- the bass swap
        // of the blend (Form.h, djOverlapBars): the outgoing kick and bass hold the floor until then, and
        // over the blend's second half the incoming percussion builds up under them, one layer at a time.
        const int kickBar = f.handover > 0 ? f.handover : kIntroKickBar;
        const bool kick = b >= kickBar;
        bp.kickBeats = kick ? 0xF : 0;
        bp.bassBeats = kick ? 0xF : 0;
        // The build-up counts *down to* the kick bar, so the last eight bars before the swap carry the
        // hat layers the groove then adds one to (the groove's rule: one layer more every eight bars).
        layers = kick ? hatLayers : (f.handover > 0 && b >= kickBar / 2 ? std::max(1, hatLayers - (kickBar - 1 - b) / 8) : 0);
        bp.quietHats = !kick;
        bp.hatLevel = kick ? 1.0f : 0.45f;
        bp.offbeatHat = kick;
        bp.shaker = b >= 8;
        if (hasPad) parts |= bPad;
        // Fills only once the kick is in: bar 24 and bar 32 of the intro close their groups.
        bp.fills = kick;
        break;
    }
    case SectionType::Groove:
        // The first core: kick and bass at full pressure, the acid as the first rhythmic lead, and one
        // percussion layer more every eight bars -- the clap on 2 and 4, then the congas, then the ride
        // (Rhythm.cpp puts them in that order behind the hats). The lead waits for drop 1.
        layers = std::min(maxLayers, hatLayers + 1 + groupInSection);
        bp.shaker = true;                     // the intro's shaker stays: layers join here, none leaves
        if (a.part[mpIndex(MelodyPart::Acid)]) parts |= bAcid;
        else if (useArp) parts |= bArp;       // no acid in this track: the arp is the rhythmic lead
        break;
    case SectionType::Build: {
        // Layers return one per four bars, the hats close up to sixteenths for the second half, the roll
        // runs over the last rollBars bars. Buildup 1 thins the bass (beats 1 and 3) for the four bars
        // before its pre-drop break; the break itself has neither kick nor bass, and its last bar stops
        // on beat 4 -- that one beat is left to a vocal or a single zap (makeFormSfx).
        const int rollStart = s.bars - std::max(0, s.rollBars);
        const int pdbStart = s.bars - std::max(0, s.pdbBars);
        layers = std::min(maxLayers, 1 + b / 4);
        if (useAcid) parts |= bAcid;
        if (useArp && b >= s.bars / 2) parts |= bArp;
        bp.hatsDense = b >= s.bars / 2;
        if (s.rollBars > 0 && b >= rollStart) { bp.rollBar = b - rollStart; bp.rollBars = s.rollBars; }
        if (s.pdbBars >= 4 && b >= pdbStart - 4 && b < pdbStart) bp.bassBeats = 0x5;
        if (s.pdbBars > 1 && b >= pdbStart && b < s.bars - 1) {
            // The pre-drop break before its last bar: kick and bass out, the roll and the hats go on.
            // BarPlan::pdb stays for the last bar alone, whose roll stops on beat 3.
            bp.kickBeats = 0;
            bp.bassBeats = 0;
            layers = std::min(layers, hatLayers);
        }
        if (s.pdbBars > 0 && b == s.bars - 1) {
            // The last bar before the drop. Variants: whole bar, half bar, beat 4 only; the fourth (the
            // kick alone on beat 4) is never drawn since 19.09.2026, and a longer pre-drop break has had
            // no kick for bars already, so there the whole bar is empty but for the roll.
            bp.pdb = true;
            static const uint8_t kKickMask[kNumPdbVariants] = { 0x0, 0x3, 0x7, 0x7 };
            static const uint8_t kBassMask[kNumPdbVariants] = { 0x0, 0x3, 0x7, 0x7 };
            const int v = s.pdbBars > 1 ? 0 : std::clamp(s.pdbVariant, 0, kNumPdbVariants - 1);
            bp.kickBeats = kKickMask[v];
            bp.bassBeats = kBassMask[v];
            parts = 0;
            layers = 0;
            bare = true;
        }
        break;
    }
    case SectionType::Drop: {
        // Everything back at once. Inside every 32-bar cycle the first sixteen bars keep one layer back
        // and bar 17 brings it in with a crash on the one -- the micro rule's "bar 16: a crash on the one
        // and a new percussion element"; the drop's own downbeat has its crash too.
        const int cycleGroup = (b % 32) / 8;
        // Drop 1 holds one layer more back than drop 2 would (19.09.2026, round "polish": the climax needs
        // room above it; kDrop1HoldDb), where the kit is big enough to still add one on bar 17.
        const int hold = !s.climax && maxLayers > 3 ? 1 : 0;
        layers = maxLayers - hold - (cycleGroup < 2 && maxLayers > 2 ? 1 : 0);
        bp.crash = b % 16 == 0;
        bp.shaker = true;
        if (useAcid) parts |= bAcid;
        if (useLead) parts |= bLead;
        if (useArp) parts |= bArp;
        if (s.climax) { bp.openHats = true; bp.ride = true; layers = maxLayers; }
        break;
    }
    case SectionType::Break: {
        // Solberg and Dibben 2019: the sudden removal of bass and bass drum -- the rule's bass vacuum. Pads
        // carry it, with one thinned melodic voice; after sixteen bars a quiet off-beat hat returns to keep
        // the tempo in the dancer's head. Goa: the arp spirals through the whole breakdown. Dark Forest:
        // two layers of percussion keep going.
        bp.kickBeats = 0;
        bp.bassBeats = 0;
        const int hatBack = std::min(16, s.bars / 2);
        layers = std::min(maxLayers, s.breakPerc);
        bp.offbeatHat = b >= hatBack;
        bp.hatLevel = 0.5f;
        if (hasPad) parts |= bPad;
        // The user's Model 3 (20.09.2026, round "dialogue"): drop 1 is the lead alone, the **main
        // breakdown is the counter-lead alone, introduced without the lead**, and drop 2 is the two of
        // them interlocking. The middle step is what makes drop 2 land, and until this round it did not
        // exist: the counter played in drop 2 only, so a listener met it for the first time already
        // interlocked with the lead. A breakdown that carries the counter therefore never carries the
        // lead -- it is an introduction, and an introduction has nothing beside it.
        //
        // Per style, through the section's own fields rather than a style index: Goa spirals its arp
        // through the whole breakdown (s.spiral), so there the counter enters at the half, over the
        // spiral; every other style has the bare pad carpet and the counter enters at the quarter --
        // in Dark Forest's sixteen-bar breakdown that is bar 5, in a 64-bar one bar 17.
        const bool counterBreak = useCounter && a.part[mpIndex(MelodyPart::Counter)];
        const int counterFrom = s.spiral ? s.bars / 2 : s.bars / 4;
        if (s.spiral && a.part[mpIndex(MelodyPart::Arp)]) parts |= bArp;
        else if (!counterBreak && b >= s.bars / 4) parts |= breakLead ? (useLead ? bLead : 0) : (useArp ? bArp : 0);
        if (counterBreak && b >= counterFrom) parts |= bCounter;
        bp.fills = false;
        if (b == 0) bp.cutBeats = s.cutBeats;
        break;
    }
    case SectionType::Outro: {
        // The inverse of the intro: a layer leaves every eight bars -- the leads at once, then the acid and
        // two percussion layers, then the pads and the rest. Since 23.09.2026 (round "DJ") the outro's last
        // `overlapTail` bars are the *blend*: the next track's intro sounds over them and owns the
        // polyphonic voices, so this track keeps its floor -- kick, bass, the acid over the blend's first
        // half, the percussion thinning by a layer every eight bars -- and nothing polyphonic; the last
        // eight bars are kick, bass and one hat, the outgoing groove alone under the incoming atmosphere.
        const int blendFrom = s.bars - std::clamp(f.overlapTail, kOutroBareBars, s.bars);
        const int bareFrom = s.bars - std::min(kOutroBareBars, s.bars);
        if (b >= bareFrom) {
            layers = 0;
            bp.offbeatHat = true;
            bare = true;
        } else if (b >= blendFrom) {
            const int blendBars = bareFrom - blendFrom, inBlend = b - blendFrom;
            const int steps = std::max(1, blendBars / 8);
            layers = std::max(1, hatLayers - (hatLayers - 1) * (inBlend / 8) / steps);
            bp.offbeatHat = true;
            if (useAcid && inBlend < blendBars / 2) parts |= bAcid;
            bare = true;   // no pad, no drone, no voice: the incoming track's own sound there
        } else {
            // Something leaves at every step: two layers, or -- where the kit has too few for that -- all
            // but the one hat of the bare end.
            const int first = std::max(hatLayers, maxLayers - 1);
            const int step = b / 8;
            layers = step == 0 ? first : (first > hatLayers ? std::max(hatLayers, first - 2) : 0);
            bp.offbeatHat = layers == 0;
            if (useAcid && step == 0) parts |= bAcid;
            if (hasPad) parts |= bPad;
        }
        bp.fills = false;
        break;
    }
    default: break;
    }
    bp.percLayers = std::clamp(layers, 0, maxLayers);

    // Pads carry what has neither lead nor acid, and join some of the rest.
    if (hasPad && !bare && s.type != SectionType::Build && s.type != SectionType::Intro) {
        if ((parts & (bAcid | bLead)) == 0 || padExtra) parts |= bPad;
    }

    // A floor under the arrangement (21.09.2026). Every rule above decides what a voice *may* do;
    // none of them guarantees that anything melodic is left. Each voice is drawn per track
    // (`present[k] = uniform() < amount`, Melody.cpp) and then thinned again per section, and the
    // draws are independent, so the combination collapses far more often than any single amount
    // suggests. Measured over four seeds, 256 bars each, as the share of bars in which a voice
    // sounds at all: acid 59/0/65/65 %, lead 0/31/31/0 %, arp 34/0/0/40 %, pad 0/31/0/8 %. Two of
    // the four tracks had no lead whatever, and in a ten-minute set the lead fell silent after bar
    // 225 and never returned -- minutes of kick, bass and percussion, which is why the tracks all
    // sounded the same. The knobs were not at fault: "Lead Amount 0.5" means half the *tracks* get a
    // lead, not that the lead plays half the time.
    //
    // So: a bar that is meant to carry music carries at least one melodic voice. Only the three
    // deliberate vacuums stay empty -- the pre-drop break, the cut after a core, and a bare bar --
    // because each of them is one or two bars long and works by being silent (Grosz et al. 2025 put
    // the pre-drop break at 1.5 to 2.5 s and the cut at 1 to 3 s).
    //
    // The breakdown and the buildup are NOT vacuums and must not be treated as such: what Solberg and
    // Dibben (2019) measured leaving a breakdown is the kick and the bass, while the pads and a
    // thinned lead carry it -- that is what the plan's own instrumentation matrix asks for. A first
    // version of this rule excluded them and left gaps of 29 to 32 bars, 53 seconds without a
    // melodic voice at 145 BPM, which is precisely the complaint it was written to answer.
    //
    // The pad is asked first because it is the carpet and disturbs the least, then the arp, then the
    // lead, then the acid. The track always has one of them: makeMelodyPlan guarantees at least one
    // of acid, lead and arp.
    //
    // 22.09.2026: **grooves and drops only.** The first version of this floor (21.09.2026) covered
    // every section that was not bare, not a pre-drop break and not a cut -- and thereby overrode
    // four rules that empty a bar *on purpose*, which the tests found and named exactly: a line in
    // the bars of the DJ overlap, where the incoming intro must add none (1601 notes); the sixteen
    // bars an intro opens with and the bare end of an outro (28 intro and 5 outro blocks off the
    // rule); notes held into a pre-drop break (40 of them, although the break's own bars were
    // excluded -- a note started in the bar before reaches into it); and, worst, 132 notes outside
    // the track's scale plus 196 arp notes off their chord's material, because a part forced into a
    // section it was not planned for plays the material of another mode than the one that section
    // borrowed.
    //
    // Those sections are not silent by accident, they are shaped. The gap the floor exists to close
    // -- 29 to 32 bars of a groove with no melodic voice at all -- is in the sections whose job is
    // to carry music, and there it still closes it. An intro that opens without a line is an intro.
    const bool carriesMusic = !bare && !bp.pdb && (s.type == SectionType::Groove || s.type == SectionType::Drop);
    if (carriesMusic && (parts & (bAcid | bLead | bArp | bPad | bStab)) == 0) {
        const MelodyPart kFloor[4] = { MelodyPart::Pad, MelodyPart::Arp, MelodyPart::Lead, MelodyPart::Acid };
        for (MelodyPart part : kFloor) {
            if (a.part[mpIndex(part)]) { parts |= partBit(part); break; }
        }
    }

    bp.padGate = (parts & bPad) != 0 && (parts & (bAcid | bLead)) != 0 && gate;

    // The new voices (19.09.2026, round "voices"), after everything above has been decided, because
    // each of them is placed *against* the others:
    //  - Counter-lead and stab share the core's eight-bar groups. The counter is the rule's "lead 2": in
    //    drop 2 it plays in every group but the first (a response needs a call before it), where the lead
    //    plays; the stab takes the groups it leaves -- elsewhere the parity of old -- so the two never
    //    sound at once. The stab joins the groove from its second group. The counter's *other* place is
    //    the main breakdown, alone and without the lead (the Break case above, 20.09.2026); buildups,
    //    intros and outros keep neither -- the pre-drop vacuum stays empty.
    //  - The drone lies under every bar in which kick and bass rest for the whole bar (the intro before
    //    the kick, the breakdown), in its low octave; in a bar where they play it may only go an octave
    //    up (the depth rule), and there it would double the pad's root and fifth or sit on the acid's
    //    octave -- so it plays there only where neither of them does.
    const bool core = isCore(s.type);
    const bool counterGroup = s.climax ? groupInSection >= 1 : (groupInSection >= 1 && (groupInSection + voiceParity) % 2 == 1);
    if (s.climax && useCounter && (parts & bLead) != 0 && counterGroup) parts |= bCounter;
    const bool stabHere = s.dry ? !(s.climax && counterGroup && (parts & bCounter) != 0) : !counterGroup;
    if (core && useStab && stabHere && (s.type == SectionType::Drop || b >= 8)) parts |= bStab;
    // The previous track's kick and bass still sound over the first bars of this one (the DJ overlap):
    // there the floor is theirs.
    // A buildup's pre-drop break is a held breath, not a floor to lay a sub under.
    bp.floorSilent = bp.kickBeats == 0 && bp.bassBeats == 0 && !bp.pdb && s.type != SectionType::Build
                  && barInTrack >= f.handover;
    // 20.09.2026, round "dialogue". The drone was measured absent: over 300 bars of the listening seed it
    // played **four notes** -- two held chords -- because it needed a bar in which neither pad nor acid
    // sounded, outside a track's first sixteen bars, outside buildups and outside outros, and a track
    // hardly ever has such a bar. The user asked for "ein tiefer, warmer Grundton-Teppich, der das
    // Frequenzvakuum fuellt", and a carpet is not a carpet where it lies in four places. So the drone now
    // lies under **every** bar of a track that has one, except the bars the rule empties on purpose (the
    // pre-drop break's last bar and the outro's bare end, both `bare`). What keeps it from muddying pad
    // and acid is no longer where it plays but *what* it plays: on a silent floor the full low chord, and
    // where kick and bass play the raised octave, quieter, and reduced to its root wherever pad or acid
    // already own the fifth (composeMelodyBar). The masking that leaves is measured, not forbidden --
    // docs/PLAN.md has the third-octave numbers.
    // 22.09.2026: what changed is only *which tracks have one*. The drone used to be drawn once per
    // track (0.6 at the knob's default), so 40 % of the night had none at all -- the user's
    // "ebensowenig wie eine Drone". Every track has one now (Melody.cpp, makeMelodyPlan). Where it
    // plays is untouched: under every bar, as the carpet argument above decided on 20.09.2026. A
    // first attempt at this round thinned it per section with the knob as the probability, and that
    // is the same carpet-in-four-places the argument rejects; `drone_amount` is therefore a switch
    // now -- above zero the track has a drone throughout.
    if (hasDrone && !bare) parts |= bDrone;
    (void)droneCore;   // the section's old coin flip; still drawn so no later draw of the section moves

    // Register (Farbood): a low-energy section drops the lead, always within the depth rule (arp from G3,
    // lead from B3). Both registers are a decision of the section, not of the bar: inside a buildup the
    // energy rises, and a voice that changed octave halfway through would jump.
    //
    // Since 19.09.2026 there is no masking rule here any more. Until then the form moved the arp up in
    // octaves until its range cleared the lead's and dropped it from any section where that passed
    // MIDI 100 -- which, with the arp held to G3..G5 and the lead to B3..A5 (Melody.h), meant every
    // section the two shared, and a drop could never carry both. The arp is now kept beside the lead
    // bar by bar, where the lead's real notes are known (Melody.cpp, "arp beside the lead"): split
    // under the lead's lowest note, else interlocked into the lead's rests.
    //
    // The arp's octave (arrangement round): up in drop 2 and only there -- the rule's "the arp an octave
    // higher" at the climax. It used to follow the energy (0.92 and up), which put it up in drop 1 as
    // well under a high arc and left drop 2 nothing of its own.
    const int leadOct = (s.energy < 0.45f && a.leadLo >= 71) ? -1 : 0;
    bp.arpOctave = static_cast<int8_t>(s.climax ? 1 : 0);
    bp.leadOctave = static_cast<int8_t>(leadOct);
    bp.climax = s.climax;
    bp.sectionBars = s.bars;
    // The main breakdown (22.09.2026): the breakdown whose next drop is the climax. It is the one
    // place the pad's harmony leaves the pendulum -- the aeolian three, or one chord held throughout.
    if (s.type == SectionType::Break) {
        for (int j = si + 1; j < f.count; ++j) {
            if (f.section[j].type != SectionType::Drop) continue;
            bp.mainBreak = f.section[j].climax;
            break;
        }
    }
    bp.parts = parts;

    // The group's figure: at beat 1 of the last bar of every eight-bar group in which the bass plays --
    // the cores as before, and since 19.09.2026 the intro after the kick and the outro, whose last
    // sixteen bars would otherwise be two identical groups of kick, bass and hat. The figures differ in
    // their last note for every bass pattern, so no two consecutive groups are identical.
    const bool figureSection = core || s.type == SectionType::Intro || s.type == SectionType::Outro;
    if (figureSection && b % 8 == 7 && (bp.bassBeats & 0x2) != 0) bp.groupFigure = kGroupFigures[figure];
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
/**
 * @brief The psychedelic layer's placement (19.09.2026, round "fx-psychedelia"), appended to what
 *        makeFormSfx placed before; every draw comes from streams of its own, so nothing that was
 *        placed before moves.
 *
 * **Around the transitions.** A sub drop under every impact (the kick's ducker keeps it off the
 * kick's transient, Engine.h) and at the cut of every breakdown, where kick and bass have just left
 * the floor to it; a reverse crash into every drop that no buildup announced and into every
 * sixteen-bar group of a drop (half the time). Never inside a buildup: its last bar is the vacuum.
 *
 * **Ear candy on free sixteenths, answering the phrase.** At the end of every two-bar group that is
 * not an eight-bar end (those keep their own candy): a squelch, a bubble, a burst of alien chatter or
 * a zap, on one of the free sixteenths of the group's last bar (2.75, 3.25, 3.5, 3.75 beats in --
 * never on a beat, which is where the kick is). The end of a four-bar group gets a larger gesture:
 * a stutter of the melodic bus on the last half beat (grooves and drops only), a bubble burst, a
 * squelch pair or a longer chatter. How likely a group gets one follows the section: drop 0.8,
 * groove 0.6, outro and breakdown 0.3, intro 0.25 -- times compose.sfx_amount and a density of the
 * track's own (0.7 .. 1.3); the palette is the track's own like the older candy's.
 *
 * **Voices** (never two at once: no two vocal events closer than four bars). A spoken phrase in the
 * intro and, in longer intros, a formant chant or another phrase; a phrase (or, one time in three, a
 * chant) after the cut of every breakdown and then one voice per eight bars (spoken, chant or chatter); a phrase on the first
 * downbeat of a buildup (the "start of builds" -- the only thing added inside a buildup, and eight
 * or more bars before its pre-drop break); in a drop now and then a voice chop into an eight-bar end.
 *
 * **The shamanic bed.** Each track picks its instruments (bowl, didgeridoo, jaw harp; at least one).
 * Intros carry a drone (didgeridoo, else jaw harp) and bowls on some of the four-bar lines; breakdowns
 * a bowl after the cut and then on about half of the four-bar lines, and drones of up to twelve bars,
 * one per sixteen, alternating between the track's two when it has both; outros a bowl every eight bars
 * and the didgeridoo; a long groove now and then eight bars of jaw harp. Drops never.
 */
static void placePsychedelia(FormPlan& f, uint64_t seed, float amount, float voiceDensity, float bedDensity)
{
    if (amount <= 0.0f) return;
    // 19.09.2026: the densities of the voices and of the bed (compose.voice_density, bed_density) scale
    // the probabilities below; every draw still happens, so at 1 every event is where it was before.
    const float dv = std::max(0.0f, voiceDensity), db = std::max(0.0f, bedDensity);
    const double bar = kBeatsPerBar;
    auto add = [&](double beat, float length, SfxType type) {
        SfxEvent e;
        e.beat = std::max(0.0, beat);
        e.length = length;
        e.type = static_cast<int>(type);
        f.sfx.push_back(e);
    };
    const float pMark = std::min(1.0f, 2.0f * amount);

    // Around the transitions.
    Rng m;
    m.seed(mixSeed(seed ^ kSaltPsy, 0));
    const size_t placed = f.sfx.size();
    for (size_t k = 0; k < placed; ++k)
        if (f.sfx[k].type == static_cast<int>(SfxType::Impact)) add(f.sfx[k].beat, 4.0f, SfxType::SubDrop);
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        const SectionType prev = i > 0 ? f.section[i - 1].type : SectionType::Intro;
        if (s.type == SectionType::Break && i > 0 && m.uniform() < pMark) add(start, static_cast<float>(2.0 * bar), SfxType::SubDrop);
        // The bass swap of the blend (23.09.2026, round "DJ"): the incoming kick's first bar gets an impact,
        // and a reverse crash runs into it -- the DJ's slam at the phrase boundary. Only inside a set
        // (handover > 0); the set's first track keeps its quiet kick entry.
        if (s.type == SectionType::Intro && f.handover > 0 && f.handover < s.bars) {
            const double swap = start + f.handover * bar;
            if (m.uniform() < pMark) add(swap, 4.0f, SfxType::Impact);
            if (m.uniform() < 0.7f * pMark) add(swap - bar, static_cast<float>(bar), SfxType::ReverseCrash);
        }
        if (s.type == SectionType::Drop && i > 0 && prev != SectionType::Build && m.uniform() < pMark)
            add(start - bar, static_cast<float>(bar), SfxType::ReverseCrash);
        if (s.type == SectionType::Drop)
            for (int b = 16; b < s.bars; b += 16)
                if (m.uniform() < 0.5f * amount) add(start + b * bar - 2.0, 2.0f, SfxType::ReverseCrash);
    }

    // Voices first, so that the chatter of the ear candy can keep its distance from them -- and from the
    // vocal makeFormSfx put on beat 4 of a pre-drop break (19.09.2026).
    std::vector<double> vocalAt;
    for (size_t k = 0; k < placed; ++k)
        if (sfxTypePart(static_cast<SfxType>(f.sfx[k].type)) == Part::Vocal) vocalAt.push_back(f.sfx[k].beat);
    auto voiceFree = [&](double beat) {
        for (double b : vocalAt) if (std::fabs(b - beat) < 4.0 * bar - 1e-9) return false;
        return true;
    };
    auto addVoice = [&](double beat, float length, SfxType type) {
        if (!voiceFree(beat)) return;
        add(beat, length, type);
        vocalAt.push_back(beat);
    };
    // A voice slot whose probability was p before the density knob: taken with min(1, p x density).
    auto voiceOdds = [&](float p) { return std::min(1.0f, p * dv); };
    Rng v;
    v.seed(mixSeed(seed ^ kSaltVoice, 0));
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        const float u0 = v.uniform(), u1 = v.uniform(), u2 = v.uniform();
        switch (s.type) {
        case SectionType::Intro:
            if (u0 < voiceOdds(0.9f * pMark)) addVoice(start + (s.bars >= 16 ? 4.0 : 2.0) * bar, 8.0f, SfxType::SpokenWord);
            if (s.bars >= 16 && u1 < voiceOdds(0.7f * amount))
                addVoice(start + 10.0 * bar, 8.0f, u2 < 0.5f ? SfxType::FormantVoice : SfxType::SpokenWord);
            break;
        case SectionType::Break: {
            if (u0 < voiceOdds(pMark)) addVoice(start + bar, 8.0f, u1 < 0.7f ? SfxType::SpokenWord : SfxType::FormantVoice);
            for (int b = 9; b + 4 <= s.bars; b += 8) {
                const float x = v.uniform(), y = v.uniform();
                if (x >= voiceOdds(0.8f * amount)) continue;
                if (y < 0.45f) addVoice(start + b * bar, 8.0f, SfxType::FormantVoice);
                else if (y < 0.8f) addVoice(start + b * bar, 8.0f, SfxType::SpokenWord);
                else addVoice(start + b * bar + 0.5, 4.0f, SfxType::AlienChatter);
            }
            break;
        }
        case SectionType::Build:
            // Until 19.09.2026 a spoken phrase opened every buildup. The user's rule puts the vocal into the
            // pre-drop break instead (makeFormSfx, beat 4 of its last bar); the draws above stay.
            break;
        case SectionType::Drop:
            if (u0 < voiceOdds(0.35f * pMark) && s.bars >= 16) {
                const int groups = s.bars / 8 - 1;   // interior eight-bar ends
                const int g = 1 + static_cast<int>(u1 * static_cast<float>(std::max(1, groups))) % std::max(1, groups);
                addVoice(start + 8.0 * g * bar - 2.0, 2.0f, SfxType::VoiceChop);
            }
            break;
        default: break;
        }
    }

    // Ear candy on free sixteenths.
    Rng c;
    c.seed(mixSeed(seed ^ kSaltPsy, 1));
    const float trackDensity = 0.7f + 0.6f * c.uniform();
    double wShort[4], wLong[4];   // squelch, bubble, chatter, zap / stutter, bubble burst, squelch pair, chatter
    for (double& x : wShort) x = c.uniform() < 0.8f ? 1.0 : 0.0;
    wShort[c.below(4)] = 2.5;
    for (double& x : wLong) x = c.uniform() < 0.8f ? 1.0 : 0.0;
    wLong[c.below(4)] = 2.5;
    static const double kFree[4] = { 2.75, 3.25, 3.5, 3.75 };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.type == SectionType::Build) continue;
        const double start = static_cast<double>(s.startBar) * bar;
        const bool groove = s.type == SectionType::Groove || s.type == SectionType::Drop;
        const float density = s.type == SectionType::Drop ? 0.8f : s.type == SectionType::Groove ? 0.6f
                            : s.type == SectionType::Intro ? 0.25f : 0.3f;
        // None of this track's candy over the blend (23.09.2026: the incoming track's candy plays there)
        // nor over the bare end (19.09.2026).
        const int bareFrom = s.type == SectionType::Outro ? s.bars - std::clamp(f.overlapTail, kOutroBareBars, s.bars) : s.bars;
        for (int b = 2; b < s.bars; b += 2) {
            if (b % 8 == 0) continue;   // the eight-bar ends keep their own candy (makeFormSfx)
            const bool four = b % 4 == 0;
            // 23.09.2026, round "SFX": the short candy at half its old chance -- the user: "die kurzen Zips und
            // Zaps [...] wiederholen sich viel zu oft und nerven" -- the four-bar gestures as they were.
            const float p = amount * density * trackDensity * (four ? 1.2f : 0.5f);
            const float roll = c.uniform();
            const int kind = drawIndex(c, four ? wLong : wShort, 4);
            const int where = c.below(4);
            if (roll >= p || b - 1 >= bareFrom) continue;
            const double last = start + (b - 1) * bar;   // the group's last bar
            if (!four) {
                switch (kind) {
                case 0: add(last + kFree[where], 0.5f, SfxType::Squelch); break;
                case 1: add(last + kFree[where], 1.0f, SfxType::Bubble); break;
                case 2: if (dv > 0.0f && voiceFree(last + 3.25)) { add(last + 3.25, 0.75f, SfxType::AlienChatter); vocalAt.push_back(last + 3.25); } break;
                default: add(last + kFree[where], 0.5f, SfxType::Zap); break;
                }
            } else {
                switch (kind) {
                case 0:
                    if (groove) add(last + 3.5, 0.5f, SfxType::Stutter);
                    else add(last + 2.75, 1.0f, SfxType::Bubble);
                    break;
                case 1: add(last + 2.75, 1.0f, SfxType::Bubble); add(last + 3.5, 0.5f, SfxType::Bubble); break;
                case 2: add(last + 3.25, 0.25f, SfxType::Squelch); add(last + 3.75, 0.25f, SfxType::Squelch); break;
                default: if (dv > 0.0f && voiceFree(last + 2.75)) { add(last + 2.75, 1.25f, SfxType::AlienChatter); vocalAt.push_back(last + 2.75); } break;
                }
            }
        }
    }

    // The shamanic bed.
    Rng t;
    t.seed(mixSeed(seed ^ kSaltBed, 0));
    bool bowl = t.uniform() < 0.65f, didge = t.uniform() < 0.55f, jaw = t.uniform() < 0.5f;
    auto bedOdds = [&](float p) { return std::min(1.0f, p * db); };
    if (!bowl && !didge && !jaw) {
        const int k = t.below(3);
        bowl = k == 0; didge = k == 1; jaw = k == 2;
    }
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        switch (s.type) {
        case SectionType::Intro: {
            // The drone of the bed is tuned to the key the engine plays in, and over the DJ overlap that is
            // still the previous track's (its kick and bass hold the floor until the hand-over): it starts
            // at the hand-over, so that it never holds one key into the other (19.09.2026).
            const SfxType drone = didge ? SfxType::Didgeridoo : SfxType::JawHarp;
            const int from = std::clamp(f.handover, 0, s.bars - 2);
            if ((didge || jaw) && t.uniform() < bedOdds(pMark)) add(start + from * bar, static_cast<float>((s.bars - 1 - from) * bar), drone);
            if (bowl) for (int b = 0; b < s.bars; b += 4) if (b == 0 ? db > 0.0f : t.uniform() < bedOdds(0.7f * pMark)) add(start + b * bar, static_cast<float>(4.0 * bar), SfxType::Bowl);
            break;
        }
        case SectionType::Break: {
            // A bowl after the cut, then now and then on a four-bar line: a strike every four bars for
            // sixty-four bars was one sound too regular to stay in the background.
            if (bowl) for (int b = 1; b < s.bars - 1; b += 4) if (t.uniform() < bedOdds(b == 1 ? pMark : 0.55f * pMark)) add(start + b * bar, static_cast<float>(4.0 * bar), SfxType::Bowl);
            // Drones of up to twelve bars from the bar after the cut, one per sixteen bars, alternating
            // between the track's two drones when it has both: a bed that comes and goes rather than
            // one sound held for a minute and a half.
            if (didge || jaw) {
                bool useDidge = didge;
                for (int b = 1; b + 5 <= s.bars - 1; b += 16) {
                    const int len = std::min(12, s.bars - 1 - b);
                    if (t.uniform() < bedOdds(pMark)) add(start + b * bar, static_cast<float>(len * bar), useDidge ? SfxType::Didgeridoo : SfxType::JawHarp);
                    if (didge && jaw) useDidge = !useDidge;
                }
            }
            break;
        }
        case SectionType::Outro: {
            // Only before the blend and the bare bars (19.09.2026; the blend since 23.09.2026).
            const int live = s.bars - std::clamp(f.overlapTail, kOutroBareBars, s.bars);
            if (bowl) for (int b = 0; b < s.bars; b += 8) if (t.uniform() < bedOdds(pMark) && b < live) add(start + b * bar, static_cast<float>(4.0 * bar), SfxType::Bowl);
            if (didge && t.uniform() < bedOdds(pMark) && live > 0) add(start, static_cast<float>(live * bar), SfxType::Didgeridoo);
            break;
        }
        case SectionType::Groove:
            if (jaw && s.bars >= 24 && t.uniform() < bedOdds(0.3f * amount)) add(start + 8.0 * bar, static_cast<float>(8.0 * bar), SfxType::JawHarp);
            break;
        default: break;
        }
    }

    // Drop 2's squelches (19.09.2026, round "arrangement"): the rule's "extra FM squelches filling
    // sixteenth gaps". In every bar of the climax, with the probability sfx_amount, one squelch on an
    // off-beat sixteenth (never on a beat, where the kick is; never on the eighth off-beat, where the hat
    // is), and in every other bar a second one. From a stream of its own: nothing above moves.
    Rng q;
    q.seed(mixSeed(seed ^ kSaltClimax, 0));
    static const double kGaps[6] = { 0.25, 0.75, 1.25, 1.75, 2.25, 3.25 };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (!s.climax) continue;
        const double start = static_cast<double>(s.startBar) * bar;
        for (int b = 0; b < s.bars; ++b) {
            const float roll = q.uniform();
            const int k0 = q.below(6), k1 = q.below(6);
            // 23.09.2026, round "SFX": 0.6 of sfx_amount per bar, the pair in one odd bar of three -- one and a
            // half squelches a bar (72 in a drop of 48) were the "Zips und Zaps [...] viiiel zu oft" the user
            // heard, and the same synthesis every time; now about 0.7 a bar, each from its own bank preset.
            if (roll >= 0.6f * amount) continue;
            add(start + b * bar + kGaps[k0], 0.25f, SfxType::Squelch);
            if (b % 2 == 1 && k1 != k0 && k1 % 3 == 0) add(start + b * bar + kGaps[k1], 0.25f, SfxType::Squelch);
        }
    }

    // 20.09.2026, round "dialogue": the user's density floor. Their rule is "im Groove gibt es nie zwei
    // Takte hintereinander ohne mindestens einen Zap, Glitch oder Swell", and the layer above does not
    // keep it: measured over the listening seed's six tracks, **90 % of the groove and drop bars lay in
    // a run of two or more bars with none of the three** (the longest run was 31 bars), and even
    // counting every short effect the generator has, 41 % did and the longest run was 15 bars. The
    // candy above is a probability per two-bar group, so a section can simply lose its coin flips.
    //
    // This pass is the floor, not another draw: it walks the grooves and the drops and, wherever a bar
    // and the bar before it both carry nothing, puts one event on a free sixteenth of it. A run of empty
    // bars can therefore never be longer than one, which is exactly the rule. Its stream is its own and
    // it only appends, so nothing placed above moves -- and where the draws above already filled the
    // bars it adds nothing at all.
    Rng gapRng;
    gapRng.seed(mixSeed(seed ^ kSaltPsy, 7));
    const int lastBar = f.count > 0 ? f.section[f.count - 1].startBar + f.section[f.count - 1].bars : 0;
    std::vector<uint8_t> carries(static_cast<size_t>(std::max(0, lastBar)), 0);
    for (const SfxEvent& e : f.sfx) {
        if (!isDensityEvent(static_cast<SfxType>(e.type))) continue;
        const int at = static_cast<int>(e.beat / bar);
        if (at >= 0 && at < lastBar) carries[static_cast<size_t>(at)] = 1;
    }
    static const double kFill[4] = { 2.75, 3.25, 3.5, 3.75 };   // the free sixteenths: never on a beat
    // 23.09.2026, round "SFX": the literature describes the effects as a layering at section boundaries and
    // in build-ups with *rising* density, not as steady fire (Psychedelic Island, "Deconstructing classic
    // psytrance tracks"; Myloops), and the user asked for fewer short zips. The floor now lets three bars
    // pass without an event and fills the fourth -- leaning long: a sweep or a swell before a zap.
    static const double kFillWeights[4] = { 0.40, 0.30, 0.15, 0.15 };   // sweep, reverse swell, zap, squelch
    static const SfxType kFillTypes[4] = { SfxType::Sweep, SfxType::ReverseSwell, SfxType::Zap, SfxType::Squelch };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.type != SectionType::Groove && s.type != SectionType::Drop) continue;
        for (int b = 0; b < s.bars; ++b) {
            const int at = s.startBar + b;
            if (at <= 2 || at >= lastBar) continue;
            bool any = false;
            for (int k = 0; k <= 3; ++k) any = any || carries[static_cast<size_t>(at - k)] != 0;
            if (any) continue;
            const int kind = drawIndex(gapRng, kFillWeights, 4);
            const int where = gapRng.below(4);
            const SfxType type = kFillTypes[kind];
            const double when = static_cast<double>(at) * bar + kFill[where];
            if (type == SfxType::Sweep) add(when, 2.0f, type);
            else if (type == SfxType::ReverseSwell) add(when, static_cast<float>(bar + 4.0 - kFill[where]), type);   // ends on the next bar's downbeat
            else add(when, 0.25f, type);
            carries[static_cast<size_t>(at)] = 1;
        }
    }
}

void makeFormSfx(FormPlan& f, uint64_t seed, float amount, float voiceDensity, float bedDensity)
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
            // The riser climbs over the last eight bars; the sweep falls into the drop, which Solberg and
            // Dibben found to be the marker listeners react to. Since 19.09.2026 (the user's rule: "on
            // beat 4 of the last bar before the drop everything stops") both end on beat 4 of that bar,
            // and beat 4 holds exactly one thing: a vocal (a voice chop) or a single laser zap, drawn per
            // buildup. The big buildup's riser climbs over sixteen bars and a second one over the last
            // four stacks on it -- "risers pitch up to the extreme". A buildup without a pre-drop break
            // (Dark Forest's first) runs its riser into the drop and leaves beat 4 alone.
            const bool stop = s.pdbBars > 0;
            const double arrive = stop ? end - 1.0 : end;
            const double rise = std::min(s.bars >= 32 ? 16.0 : 8.0, static_cast<double>(s.bars)) * bar;
            const bool dry = i + 1 < f.count && f.section[i + 1].dry;
            if (i > 0 && mark()) add(start - bar, static_cast<float>(bar), SfxType::ReverseSwell);
            if (mark()) add(arrive - rise, static_cast<float>(rise), SfxType::Riser);
            const bool voice = r.uniform() < 0.5f;
            if (mark() && stop) add(end - 1.0, 1.0f, voice ? SfxType::VoiceChop : SfxType::Zap);
            if (mark() && !dry) add(arrive - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep);
            if (mark() && !dry) add(end, 4.0f, SfxType::Impact);
            if (s.bars >= 32 && mark()) add(arrive - 4.0 * bar, static_cast<float>(4.0 * bar), SfxType::Riser);
        } else if (s.type == SectionType::Break) {
            if (i > 0 && mark()) add(start - bar, static_cast<float>(bar), SfxType::ReverseSwell);
            if (mark()) add(start, static_cast<float>(4.0 * bar), SfxType::Downlifter);
        } else if (s.type == SectionType::Drop && i > 0 && prev != SectionType::Build) {
            // A drop that no buildup announced (a groove straight into it).
            if (mark()) add(start - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep);
            if (mark() && !s.dry) add(start, 4.0f, SfxType::Impact);
        } else if (s.type == SectionType::Outro) {
            // Into the outro a downlifter. Until 19.09.2026 an eight-bar sweep closed the track to mask the
            // key change; the outro's last sixteen bars are now kick, bass and a hat only, the key changes
            // with the next track's kick and bass at the hand-over, and nothing is left there to mask.
            if (i > 0 && mark()) add(start, static_cast<float>(4.0 * bar), SfxType::Downlifter);
        } else if (s.type == SectionType::Intro && s.bars > kIntroKickBar && mark()) {
            // The inhale before the kick enters on bar 17 of the intro.
            add(start + (kIntroKickBar - 1) * bar, static_cast<float>(bar), SfxType::ReverseSwell);
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
        // The outro's bare bars (kick, bass, a hat) carry nothing else.
        const double end = static_cast<double>(s.startBar + s.bars - (s.type == SectionType::Outro ? std::min(kOutroBareBars, s.bars) : 0)) * bar;
        // Interior group boundaries only: the section's own ends belong to the transition markers.
        for (double g = start + period; g < end - 1e-9; g += period) {
            const bool cycleEnd = std::fmod(g - start, 32.0 * bar) < 1e-9;
            const float roll = c.uniform();
            // The micro rule's bar 32 (19.09.2026): the end of every 32-bar cycle inside a section carries
            // a marker for certain (as the transitions do, from sfx_amount 0.5 up): a one-bar downlifter
            // or, in a core, a glitch -- the stutter of the melodic bus on the last half beat.
            if (cycleEnd && roll < pMark) {
                if (isCore(s.type) && roll < 0.5f * pMark) add(g - 0.5, 0.5f, SfxType::Stutter);
                else add(g - bar, static_cast<float>(bar), SfxType::Downlifter);
                continue;
            }
            if (roll >= amount) continue;
            switch (drawIndex(c, w, 4)) {
            case 0: add(g - 1.0, 0.5f, SfxType::Zap); break;                                        // beat 4
            case 1: add(g - bar, static_cast<float>(bar), SfxType::Sweep); break;                   // filtered noise, up and down
            case 2: add(g - bar, static_cast<float>(bar), SfxType::ReverseSwell); break;            // into the downbeat
            default: add(g - 2.0 * bar, static_cast<float>(2.0 * bar), SfxType::Sweep); break;      // a longer wash
            }
        }
    }
    // The psychedelic layer of 19.09.2026, from streams of its own (placePsychedelia above).
    placePsychedelia(f, seed, amount, voiceDensity, bedDensity);

    // Atmospheres (23.09.2026, round "SFX"; Sfx.h, SfxType::Atmosphere): the background layer the literature
    // names beside risers and impacts -- pads and atmospheres that take over in breakdowns and sit under an
    // intro (Psychedelic Island; Myloops). Long events, two to eight bars, ending on a phrase boundary: in the
    // intro and the outro on every eight-bar line, in the breakdown too, in a groove or a drop once per
    // sixteen bars from its ninth bar. Never in a build-up (the riser owns it), never over the outro's blend
    // (composeSfxBar cuts this track's events there; the incoming track's atmospheres take the room).
    {
        Rng at;
        at.seed(mixSeed(seed ^ kSaltSfx, 0x41544Dull));
        const float pAtmo = std::min(1.0f, 1.5f * amount);
        for (int i = 0; i < f.count; ++i) {
            const Section& s = f.section[i];
            const double start = static_cast<double>(s.startBar) * bar;
            if (s.type == SectionType::Intro || s.type == SectionType::Outro || s.type == SectionType::Break) {
                // The outro's last kDjOverlapMax bars are the next track's (the blend, whatever length it turns
                // out to have): no atmosphere of this track starts there.
                const int until = s.type == SectionType::Outro ? s.bars - kDjOverlapMax : s.bars;
                for (int b = 0; b < until; b += 8) {
                    const int len = std::min(8, until - b);
                    if (at.uniform() < (s.type == SectionType::Break ? 0.9f : 0.75f) * pAtmo) add(start + b * bar, static_cast<float>(len * bar), SfxType::Atmosphere);
                }
            } else if (s.type == SectionType::Groove || s.type == SectionType::Drop) {
                for (int b = 8; b + 4 <= s.bars; b += 16) {
                    const int len = 4 + 4 * at.below(2);
                    if (at.uniform() < 0.5f * pAtmo) add(start + b * bar, static_cast<float>(std::min(len, s.bars - b) * bar), SfxType::Atmosphere);
                }
            }
        }
    }

    // The bank presets (23.09.2026, round "SFX"; Sfx.h, SfxPreset). Every event of the effects strip gets one
    // of its type's family, drawn from the form's own stream, and no preset plays twice in one track -- the
    // user's "wiederholen sich viel zu oft" was exactly that. A track with more events of a type than the
    // family has presets starts a second round. The pick rides in the event's variant, which the score's
    // lane carries to the engine (Engine.cpp, Sfx::trigger).
    {
        Rng pr;
        pr.seed(mixSeed(seed ^ kSaltSfx, 0x505245ull));
        std::vector<std::vector<int>> used(static_cast<size_t>(kNumSfxTypes));
        for (SfxEvent& e : f.sfx) {
            const int t = std::clamp(e.type, 0, kNumSfxTypes - 1);
            if (sfxTypePart(static_cast<SfxType>(t)) != Part::Sfx) continue;
            const int n = kSfxBankCount[t];
            if (n <= 0) continue;
            std::vector<int>& u = used[static_cast<size_t>(t)];
            if (static_cast<int>(u.size()) >= n) u.clear();
            int pick = pr.below(n);
            while (std::find(u.begin(), u.end(), pick) != u.end()) pick = (pick + 1) % n;
            u.push_back(pick);
            e.variant = static_cast<uint16_t>(pick + 1);
        }
    }
    std::stable_sort(f.sfx.begin(), f.sfx.end(), [](const SfxEvent& a, const SfxEvent& b) { return a.beat < b.beat; });
    // Which phrase or bed variant each voice and bed event plays (19.09.2026): from the form seed, in the
    // order of the events, from a generator of its own -- nothing placed above moves. Until then the
    // engine derived it from the event's beat, so a phrase was a function of where it fell.
    Rng vr;
    vr.seed(mixSeed(seed ^ kSaltVariant, 0));
    for (SfxEvent& e : f.sfx)
        if (sfxTypePart(static_cast<SfxType>(e.type)) != Part::Sfx) e.variant = static_cast<uint8_t>(1 + vr.below(255));
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
                       float base0, float base1, bool knobs, std::vector<ControlEvent>& out, float resoBase, float decayBase)
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
        // Over the roll (19.09.2026: the section's own roll, sixteen bars in the big buildup).
        const double rollBars = std::min(static_cast<double>(s.rollBars > 0 ? s.rollBars : kRollBars), bars);
        push(mb + mix::PercHall, kRollSend, beat + (bars - rollBars) * kBeatsPerBar,
             static_cast<float>(rollBars * kBeatsPerBar));
    }
    if (knobs) return;

    // The modulation effects' ride (19.09.2026, round "fx-psychedelia"; PsyFx.h): the section half of
    // their automation, the event half being the engine's (Engine.h). Per section type, the depth drawn
    // from the section's own seed: a drop opens the flanger on the SFX bus, a breakdown the phaser and
    // a slow drift of the shifter, a buildup drags the shifter up over its whole length (with its riser
    // on top, the event motion), and the drop snaps it back to zero on its downbeat.
    {
        const int xb = p.base(Module::PsyFx);
        Rng fx;
        fx.seed(mixSeed(seed ^ kSaltFxRide, 0));
        const float a = fx.uniform(), b = fx.uniform(), sign = fx.below(2) == 0 ? 1.0f : -1.0f;
        const float perHz = 1.0f / (p.desc(xb + psyfx::ShiftHz).maxValue - p.desc(xb + psyfx::ShiftHz).minValue);
        const float twoBars = static_cast<float>(2.0 * kBeatsPerBar);
        float flange = 0.0f, phase = 0.0f, shiftHz = 0.0f, shiftLen = twoBars;
        switch (s.type) {
        case SectionType::Drop:   flange = 0.15f + 0.20f * a; shiftLen = 0.0f; break;
        case SectionType::Groove: flange = 0.10f * a; phase = 0.10f * b; break;
        case SectionType::Break:  phase = 0.25f + 0.20f * a; shiftHz = sign * (10.0f + 30.0f * b); shiftLen = static_cast<float>(bars * kBeatsPerBar); break;
        case SectionType::Build:  flange = 0.15f * a; shiftHz = 60.0f + 60.0f * b; shiftLen = static_cast<float>(bars * kBeatsPerBar); break;
        default:                  phase = 0.15f * a; shiftHz = sign * 8.0f * b; break;
        }
        push(xb + psyfx::FlangerMix, flange, beat, twoBars);
        push(xb + psyfx::PhaserMix, phase, beat, twoBars);
        if (shiftLen > 0.0f && (s.type == SectionType::Build || s.type == SectionType::Break))
            push(xb + psyfx::ShiftHz, 0.0f, beat, 0.0f);   // from zero, so the ramp is the section's own
        push(xb + psyfx::ShiftHz, shiftHz * perHz, beat, shiftLen);
    }

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
        push(ab + acid::Resonance, resoBase - depth * kRideReso, beat, len);
        push(ab + acid::Cutoff, base1, beat + half * kBeatsPerBar, len);
        push(ab + acid::Resonance, resoBase, beat + half * kBeatsPerBar, len);
        return;
    }
    // Everything else rides the four-stage cycle (Form.h, RideShape): 32 bars, or the whole section
    // when it is shorter, so that a buildup's cycle ends in the dive exactly on the drop.
    const RideShape shape = acidRideShape(bars);
    // 19.09.2026 (round "voices"): the ride is an excursion around the *voiced* resonance and decay --
    // the knob plus the track's acid voicing (resoBase, decayBase) -- instead of around the knob, so a
    // track's voicing can own the two quantities the section rides. With both bases at zero (the first
    // track, which plays the knobs) every event below is the one it was.
    const int resoId = ab + acid::Resonance, decayId = ab + acid::Decay;
    const double knobReso = p.get(resoId);
    const double voicedReso = p.fromNormalised(resoId, p.toNormalised(resoId, p.get(resoId)) + resoBase);
    const double knobDecay = p.get(decayId);
    const double voicedDecay = p.fromNormalised(decayId, p.toNormalised(decayId, p.get(decayId)) + decayBase);
    const ParamDesc& dd = p.desc(decayId);
    // Decay is a log parameter: a ratio of the voiced decay is a fixed normalised distance from it.
    const double decaySpan = std::log(static_cast<double>(dd.maxValue) / dd.minValue);
    auto decayOffset = [&](double ratio) {
        const double target = std::clamp(voicedDecay * ratio, static_cast<double>(dd.minValue), static_cast<double>(dd.maxValue));
        return static_cast<float>(std::log(target / knobDecay) / decaySpan);
    };
    const float resoMedium = static_cast<float>(std::min(voicedReso, static_cast<double>(kRideResoMedium)) - knobReso);
    const float resoSquelch = static_cast<float>(std::max(voicedReso, static_cast<double>(kRideResoSquelch)) - knobReso);
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
        // The micro rule's bar 24 (19.09.2026): a short sweep on the acid -- up by kRideSweep half-
        // excursions over the first half of the cycle's 24th bar and back onto the ride's line over the
        // second, arriving where the ride itself arrives at the end of stage 3. Only in full 32-bar cycles.
        if (shape.length >= 32.0) {
            const double b24 = shape.stage[3] - 1.0;
            push(ab + acid::Cutoff, base(c0 + b24 + 0.5) + h * (shape.cutoff[3] + kRideSweep), at(b24), len(0.5));
            push(ab + acid::Cutoff, cut(3), at(b24 + 0.5), len(0.5));
        }
        // Resonance: medium through stages 1 and 2, up to the squelch over stage 3, held to the end.
        push(ab + acid::Resonance, resoMedium, at(0.0), len(1.0));
        push(ab + acid::Resonance, resoSquelch, at(shape.stage[2]), len(shape.stage[3] - shape.stage[2]));
        // Decay: short and dry in stage 1, back to the knob over stage 2, longer through 3 and 4.
        push(ab + acid::Decay, decayOffset(kRideDecayShort), at(0.0), len(1.0));
        push(ab + acid::Decay, decayOffset(1.0), at(shape.stage[1]), len(shape.stage[2] - shape.stage[1]));
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
