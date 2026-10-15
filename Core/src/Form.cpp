/**
 * @file Form.cpp
 * @brief Style profiles, the energy arc, the form grammar and the instrumentation matrix
 *        (where the effects go is FormSfx.cpp's).
 */
#include "phos/Form.h"
#include "phos/Preferences.h"
#include "phos/Dsp.h"
#include "phos/Sfx.h"
#include "phos/Util.h"
#include <algorithm>
#include <cmath>

#include "Salts.h"
using namespace phos::salts::form;   // this file's seed salts (Salts.h)

namespace phos {

// StyleNames and ArcNames are defined in Params.cpp with the other choice tables, so that the
// parameter table can be built without linking the form.
const char* const PdbVariantNames[NumPdbVariants] = { "Full bar", "Half bar", "Beat 4", "Kick on 4" };

// The bass figures a group may end on. They are the rows of Composer's figure table whose *last*
// note differs for every bass pattern (the short patterns play only that note), so two consecutive
// eight-bar groups can never hold the same bass notes. -1 means "no figure", which is the fourth
// distinct value.
const int GroupFigures[4] = { 0, 1, 3, -1 };

namespace {

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
// (Form.cpp, Templates) with certainty: the rule text gives every subgenre one form. The fourth PDB variant
// (the kick alone on beat 4) is never drawn any more: "on beat 4 of the last bar before the drop everything
// stops". breakShare and introBars are no longer read (the templates fix both).
const StyleProfile Profiles[NumStyles] = {
    // Goa
    { "Goa", 145.0, 3.0,
      { 0.15, 0.20, 0.15, 0.30, 0.15, 0.05 },          // scales: Phrygian dominant and double harmonic lead
      { 0.0, 0.0, 1.0, 0.0 },                          // template: Goa
      { 0.0, 0.5, 0.0, 0.0, 0.0, 0.2, 0.0, 0.1, 0.0, 0.0, 0.4, 0.0 },   // i-bII and i-bVII
      { 1.15f, 1.10f, 1.10f, 1.25f, 0.80f, 1.20f, 1.20f }, 0.9f, 0.95f,
      { 0.20, 0.25, 0.55, 0.00 },                      // mostly beat 4 alone: rarely total silence
      { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.26f, 1.0f, 16.0f,
      { 0.15, 0.25, 0.10, 0.30, 0.15, 0.05 }, 0.45f, StyleIdE::GOA,
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
      { 0.30, 0.30, 0.15, 0.15, 0.05, 0.05 }, 0.30f, StyleIdE::FULL_ON,
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
      { 0.45, 0.05, 0.00, 0.00, 0.00, 0.50 }, 0.12f, StyleIdE::PROGRESSIVE,
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
      { 0.20, 0.40, 0.30, 0.10, 0.00, 0.00 }, 0.35f, StyleIdE::DARK_FOREST,
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
      { 0.20, 0.35, 0.30, 0.15, 0.00, 0.00 }, 0.40f, StyleIdE::HI_TECH,
      // lead vector (22.09.2026): densest, freest, hocketing against the bass, hardly a slide
      { 0.90f, 0.70f, 0.60f, 0.40f, 1.30f, 0.10f, 0.50f, 0.80f, 0.07f,
        { 0.25, 0.10, 0.15, 0.20, 0.30 },   // archetypes: surge, arch, pedal, cascade, tension call
        { 0.0, 0.05, 0.20, 0.20, 0.15, 0.30, 0.10 },   // cell operators: keep, cadence, up, down, invert end, shift, thin
        7, { 0.3, 0.1, 0.0, 0.6 } } },   // register shift, counter modes (echo, answer, timbral, hocket) -- Hi-Tech: G4 .. F#5, hocket
};

/** @brief Control points of the arcs: (t, E) pairs, interpolated with raised cosines. */
struct ArcPoint {
    double m_t;   ///< where in the set, 0..1
    double m_e;   ///< the energy there
};
const ArcPoint WarmUp[]   = { { 0.0, 0.25 }, { 0.35, 0.45 }, { 0.75, 0.70 }, { 1.0, 0.85 } };   ///< the warm-up: rising
const ArcPoint PeakTime[] = { { 0.0, 0.70 }, { 0.30, 0.95 }, { 0.65, 1.00 }, { 1.0, 0.85 } };   ///< peak time: high from early on
const ArcPoint Morning[]  = { { 0.0, 0.80 }, { 0.30, 0.70 }, { 0.70, 0.55 }, { 1.0, 0.45 } };   ///< the morning: falling slowly
const ArcPoint Closing[]  = { { 0.0, 0.90 }, { 0.25, 0.75 }, { 0.70, 0.45 }, { 1.0, 0.20 } };   ///< the closing: falling
const ArcPoint Flat[]     = { { 0.0, 0.70 }, { 1.0, 0.70 } };   ///< flat

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
enum : int { SLOT_INTRO = 0, SLOT_GROOVE, SLOT_BUILD1, SLOT_DROP1, SLOT_BREAK, SLOT_BUILD2, SLOT_DROP2, SLOT_OUTRO, SLOTS };
/** @brief One form template: the slots' lengths at 256 bars and how the track grows past them. */
struct Template {
    int m_bars[SLOTS];      ///< length of each slot at 256 bars
    int m_grow[8];           ///< the slots that take 16 more bars when the track is longer, in order (-1 ends)
    int m_growCap[8];        ///< the length each of those grows to at most
};
/** @brief each slot's section type */
const SectionTypeE SlotType[SLOTS] = { SectionTypeE::INTRO, SectionTypeE::GROOVE, SectionTypeE::BUILD, SectionTypeE::DROP,
                                        SectionTypeE::BREAK, SectionTypeE::BUILD, SectionTypeE::DROP, SectionTypeE::OUTRO };
/** @brief The form templates, one per body. */
const Template Templates[NumBodies] = {
    // Full-On (and Hi-Tech): the strict two-drop form.
    { { 32, 32, 16, 32, 32, 32, 48, 32 }, { SLOT_DROP2, SLOT_DROP1, SLOT_BREAK, SLOT_GROOVE, SLOT_DROP1, SLOT_BREAK, SLOT_GROOVE, -1 },
      { 64, 48, 48, 48, 64, 64, 64, 0 } },
    // Progressive: the 64-bar groove, a short big buildup, a 32-bar drop 2.
    { { 32, 64, 16, 32, 32, 16, 32, 32 }, { SLOT_DROP2, SLOT_DROP1, SLOT_BREAK, SLOT_DROP2, SLOT_DROP1, SLOT_BREAK, -1, -1 },
      { 48, 48, 48, 64, 64, 64, 0, 0 } },
    // Goa: the Full-On lengths, with the spiral in the breakdown.
    { { 32, 32, 16, 32, 32, 32, 48, 32 }, { SLOT_DROP2, SLOT_BREAK, SLOT_DROP1, SLOT_GROOVE, SLOT_BREAK, SLOT_DROP1, SLOT_GROOVE, -1 },
      { 64, 48, 48, 48, 64, 64, 64, 0 } },
    // Dark Forest: the long drop 1, the short breakdown.
    { { 32, 32, 16, 48, 16, 32, 48, 32 }, { SLOT_DROP2, SLOT_DROP1, SLOT_GROOVE, SLOT_BREAK, SLOT_GROOVE, SLOT_BREAK, -1, -1 },
      { 64, 64, 48, 32, 64, 48, 0, 0 } },
};
/** @brief The order in which slots give up 16 bars when the track is shorter, and how far (all templates). */
const int Shrink[9][2] = { { SLOT_DROP2, 32 }, { SLOT_GROOVE, 16 }, { SLOT_BREAK, 16 }, { SLOT_DROP1, 16 },
                            { SLOT_BUILD2, 16 }, { SLOT_INTRO, 16 }, { SLOT_OUTRO, 16 }, { SLOT_DROP2, 16 }, { SLOT_GROOVE, 16 } };

/** @brief True while every hard constraint holds (exposed as formConstraintsHold; Form.h says which). */
bool constraintsHoldImpl(const FormPlan& f)
{
    int climaxes = 0, lastDrop = -1;
    for (int i = 0; i < f.m_count; ++i) {
        const Section& s = f.m_section[i];
        if (s.m_bars < 8 || s.m_bars % 8 != 0 || s.m_startBar % 8 != 0) return false;
        if (isCore(s.m_type) && s.m_bars < 16) return false;
        if (s.m_type == SectionTypeE::BUILD && (s.m_bars < 8 || s.m_bars > 32)) return false;
        if (s.m_type == SectionTypeE::BREAK && s.m_bars < 16) return false;
        if (s.m_type == SectionTypeE::DROP) lastDrop = i;
        if (s.m_climax) ++climaxes;
    }
    if (f.m_count < 2 || f.m_section[0].m_type != SectionTypeE::INTRO || f.m_section[0].m_bars < DjOverlap) return false;
    if (f.m_section[f.m_count - 1].m_type != SectionTypeE::OUTRO || f.m_section[f.m_count - 1].m_bars < DjOverlap) return false;
    return climaxes == 1 && lastDrop >= 0 && f.m_section[lastDrop].m_climax;
}

/** @brief Recomputes the start bars and the total after a length changed. */
void relayout(FormPlan& f)
{
    int at = 0;
    for (int i = 0; i < f.m_count; ++i) { f.m_section[i].m_startBar = at; at += f.m_section[i].m_bars; }
    f.m_bars = at;
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
    for (int k = 0; k < SLOTS; ++k) { bars[k] = t.m_bars[k]; total += bars[k]; }
    for (bool moved = true; total < target && moved;) {
        moved = false;
        for (int g = 0; g < 8 && t.m_grow[g] >= 0 && total < target; ++g)
            if (bars[t.m_grow[g]] + 16 <= t.m_growCap[g]) { bars[t.m_grow[g]] += 16; total += 16; moved = true; }
    }
    for (int k = 0; k < 9; ++k) {
        const int slot = Shrink[k][0], floor = Shrink[k][1];
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
 *
 * **The arc's move (23.09.2026, round "Set-Kurve").** @p arc is the set's energy where the track lies
 * (the mean of arcIn and arcOut). At the night's peak (0.75 and above) half the tracks give eight bars of
 * the main breakdown to drop 2 -- the climax grows where the floor wants it; in the warm-up or the closing
 * (0.35 and below) half the tracks do the reverse, a longer breakdown and a shorter climax. Drawn after the
 * fuzz moves from the same stream, so those stay what they were; the Flat arc (0.70) draws nothing.
 */
void jitterForm(int* bars, uint64_t seed, double arc)
{
    static const int Min[SLOTS] = { DjOverlap, 16, 8, 24, 16, 8, 24, DjOverlap };
    static const int Max[SLOTS] = { 48, 96, 32, 64, 64, 32, 64, 48 };
    static const int Pairs[7][2] = { { SLOT_INTRO, SLOT_GROOVE }, { SLOT_GROOVE, SLOT_BUILD1 }, { SLOT_BUILD1, SLOT_DROP1 }, { SLOT_DROP1, SLOT_BREAK },
                                      { SLOT_BREAK, SLOT_BUILD2 }, { SLOT_BUILD2, SLOT_DROP2 }, { SLOT_DROP2, SLOT_OUTRO } };
    Rng j;
    j.seed(mixSeed(seed ^ SaltFuzz, 0));
    const int moves = j.uniform() < 0.3f ? 2 : 1;
    for (int m = 0; m < moves; ++m) {
        const int pair = j.below(7), dir = j.below(2);
        const int from = Pairs[pair][dir], to = Pairs[pair][1 - dir];
        if (bars[from] - 8 >= Min[from] && bars[to] + 8 <= Max[to]) { bars[from] -= 8; bars[to] += 8; }
    }
    if (arc >= 0.75 || arc <= 0.35) {
        const bool peak = arc >= 0.75;
        if (j.uniform() < 0.5f) {
            const int from = peak ? SLOT_BREAK : SLOT_DROP2, to = peak ? SLOT_DROP2 : SLOT_BREAK;
            if (bars[from] - 8 >= Min[from] && bars[to] + 8 <= Max[to]) { bars[from] -= 8; bars[to] += 8; }
        }
    }
}

} // namespace

bool formConstraintsHold(const FormPlan& f) { return constraintsHoldImpl(f); }

const StyleProfile& styleProfile(StyleIdE id)
{
    const int i = static_cast<int>(id);
    return Profiles[i < 0 || i >= NumStyles ? static_cast<int>(StyleIdE::FULL_ON) : i];
}

StyleIdE styleOf(const ParamStore& p)
{
    return static_cast<StyleIdE>(std::clamp(p.getInt(p.base(ModuleE::COMPOSE) + compose::STYLE), 0, NumStyles - 1));
}

ArcIdE arcOf(const ParamStore& p)
{
    return static_cast<ArcIdE>(std::clamp(p.getInt(p.base(ModuleE::COMPOSE) + compose::ARC), 0, NumArcs - 1));
}

double arcEnergy(ArcIdE arc, double t)
{
    const ArcPoint* pts = Flat;
    int n = 2;
    switch (arc) {
    case ArcIdE::WARM_UP:   pts = WarmUp;   n = 4; break;
    case ArcIdE::PEAK_TIME: pts = PeakTime; n = 4; break;
    case ArcIdE::MORNING:  pts = Morning;  n = 4; break;
    case ArcIdE::CLOSING:  pts = Closing;  n = 4; break;
    default: break;
    }
    t = std::clamp(t, 0.0, 1.0);
    for (int i = 0; i + 1 < n; ++i) {
        if (t > pts[i + 1].m_t) continue;
        const double span = pts[i + 1].m_t - pts[i].m_t;
        const double u = span > 0.0 ? (t - pts[i].m_t) / span : 0.0;
        // Raised cosine: value and slope are continuous at every control point.
        const double w = 0.5 * (1.0 - std::cos(3.141592653589793 * u));
        return pts[i].m_e + (pts[i + 1].m_e - pts[i].m_e) * w;
    }
    return pts[n - 1].m_e;
}

float typeEnergy(SectionTypeE type)
{
    switch (type) {
    case SectionTypeE::INTRO:  return 0.40f;
    case SectionTypeE::GROOVE: return 0.70f;
    case SectionTypeE::BUILD:  return 0.75f;   // the start; it rises to the drop's energy
    case SectionTypeE::DROP:   return 1.00f;
    case SectionTypeE::BREAK:  return 0.30f;
    case SectionTypeE::OUTRO:  return 0.45f;
    default:                  return 0.50f;
    }
}

/**
 * @brief The mode one section borrows over the tonic pedal, or @p trackScale when it borrows nothing.
 *
 * The style profile says which modes are available at all and how often a section borrows; the
 * section's energy says how far it may reach, by multiplying each mode's weight with
 * @c exp(InterchangeEnergy * (energy - 0.7) * colourTones(mode)). At a drop (energy near 1) that
 * lifts double harmonic by a factor of about 1.6 and Phrygian dominant by 1.4 over Aeolian; in a
 * breakdown (0.3) it presses them down by the same amount, so the Hijaz colour arrives where the
 * literature puts it -- at the peak (Easwaran 2004) -- rather than at random.
 *
 * Everything hangs off the section's own seed, because a section is a lockable unit (PLAN 6.8).
 */
static int borrowedMode(const StyleProfile& s, uint64_t sectionSeed, int trackScale, float energy)
{
    if (s.m_interchangeChance <= 0.0f) return trackScale;
    Rng r;
    r.seed(mixSeed(sectionSeed ^ SaltMode, 0));
    if (r.uniform() >= s.m_interchangeChance) return trackScale;
    double w[NumScales];
    double total = 0.0;
    for (int m = 0; m < NumScales; ++m) {
        w[m] = s.m_interchangeWeight[m] * std::exp(InterchangeEnergy * (static_cast<double>(energy) - 0.7)
                                                 * scaleColourTones(m));
        if (m == trackScale) w[m] = 0.0;   // "borrowing" the mode the track already has is not a borrow
        total += w[m];
    }
    if (total <= 0.0) return trackScale;
    return drawIndex(r, w, NumScales);
}

FormPlan makeFormPlan(const StyleProfile& s, uint64_t seed, int target, double arcIn, double arcOut,
                      int trackScale, const uint64_t* sectionSeed)
{
    Rng r;
    r.seed(mixSeed(seed, SaltForm));
    FormPlan f;
    target = std::clamp((target / TrackBarStep) * TrackBarStep, MinTrackBars, MaxTrackBars);

    // The template is the style's (19.09.2026: the rule text gives every subgenre one form; the draw
    // stays a draw so that a profile may one day mix them).
    // The listener's preferences reweight the choice (Preferences.h; factors of exactly 1 without them).
    double bodyWeight[NumBodies];
    for (int i = 0; i < NumBodies; ++i) bodyWeight[i] = s.m_bodyWeight[i] * preferenceFactor("form.template", FormTemplateFeatureNames[i]);
    f.m_body = drawIndex(r, bodyWeight, NumBodies);
    const Template& tp = Templates[f.m_body];
    int lens[SLOTS];
    fitTemplate(tp, target, lens);
    jitterForm(lens, seed, 0.5 * (arcIn + arcOut));   // the form's fuzziness, and the arc's move (23.09.2026)
    f.m_count = SLOTS;
    for (int k = 0; k < SLOTS; ++k) {
        f.m_section[k].m_type = SlotType[k];
        f.m_section[k].m_bars = lens[k];
    }
    relayout(f);

    // The form's own decisions per slot (Form.h, Section): the rolls, the pre-drop breaks, what the
    // styles do differently.
    Section& build1 = f.m_section[SLOT_BUILD1];
    Section& build2 = f.m_section[SLOT_BUILD2];
    Section& brk = f.m_section[SLOT_BREAK];
    f.m_section[SLOT_DROP2].m_climax = true;
    // Buildup 1: a four-bar roll inside the four-bar pre-drop break (bars 77-80 of the rule), except in
    // Dark Forest, whose first drop comes without the conventional stop.
    build1.m_rollBars = std::min(4, build1.m_bars);
    build1.m_pdbBars = f.m_body == 3 ? 0 : std::min(4, build1.m_bars);
    // The big buildup: the sixteen-bar roll (quarters, eighths, sixteenths, thirty-seconds, four bars
    // each) over its second half, and the stop on beat 4 of its last bar. A short big buildup (16 bars,
    // Progressive) rolls over its last eight.
    build2.m_rollBars = std::min(16, build2.m_bars / 2);
    build2.m_pdbBars = 1;
    if (f.m_body == 1) { f.m_section[SLOT_DROP1].m_dry = true; f.m_section[SLOT_DROP2].m_dry = true; }
    if (f.m_body == 2) brk.m_spiral = true;
    if (f.m_body == 3) brk.m_breakPerc = 2;

    // Energies from the arc, and the per-section decisions that belong to the form.
    Rng d;
    d.seed(mixSeed(seed ^ SaltSection, 0));
    auto scaled = [](float nominal, double arc) { return static_cast<float>(nominal * (0.55 + 0.45 * arc)); };
    for (int i = 0; i < f.m_count; ++i) {
        Section& sec = f.m_section[i];
        const double t0 = f.m_bars > 0 ? static_cast<double>(sec.m_startBar) / f.m_bars : 0.0;
        const double t1 = f.m_bars > 0 ? static_cast<double>(sec.m_startBar + sec.m_bars) / f.m_bars : 1.0;
        const double a0 = arcIn + (arcOut - arcIn) * t0, a1 = arcIn + (arcOut - arcIn) * t1;
        // The arc scales the type's nominal energy; at arc 0 a section keeps 55 % of it, so the order
        // drop > buildup > groove > breakdown survives every arc. Drop 1 stands at Drop1Share of a drop:
        // the rule makes drop 2 the climax, not a repeat.
        const float nominal = typeEnergy(sec.m_type) * (sec.m_type == SectionTypeE::DROP && !sec.m_climax ? Drop1Share : 1.0f);
        sec.m_energy = scaled(nominal, a0);
        sec.m_energyTo = scaled(nominal, a1);
        if (sec.m_type == SectionTypeE::DROP) sec.m_energyTo = sec.m_energy;   // a drop holds its level
        if (sec.m_type == SectionTypeE::BUILD) sec.m_pdbVariant = drawIndex(d, s.m_pdbWeight, NumPdbVariants);
        // The cut of Grosz et al. (1 to 3 s): the bass vacuum of the rule, "kick and bass cut at once".
        // Goa's breakdown is never silent, Dark Forest's keeps its percussion; neither takes a cut.
        if (sec.m_type == SectionTypeE::BREAK && i > 0 && isCore(f.m_section[i - 1].m_type)) {
            const float cut = d.uniform() < 0.5f ? 1.0f : 2.0f;
            sec.m_cutBeats = (sec.m_spiral || sec.m_breakPerc > 0) ? 0.0f : cut;
        }
    }
    // Drop 2 is the climax: its energy stands ClimaxMargin above every other section's, whatever the
    // arc does across the track (a Closing arc falls towards the end and would otherwise put drop 1 on
    // top). Where the margin would take it past 1, the others come down instead.
    {
        Section& peak = f.m_section[SLOT_DROP2];
        float others = 0.0f;
        for (int i = 0; i < f.m_count; ++i) if (i != SLOT_DROP2) others = std::max({ others, f.m_section[i].m_energy, f.m_section[i].m_energyTo });
        peak.m_energy = peak.m_energyTo = std::min(1.0f, std::max(peak.m_energy, others + ClimaxMargin));
        for (int i = 0; i < f.m_count; ++i) {
            if (i == SLOT_DROP2) continue;
            f.m_section[i].m_energy = std::min(f.m_section[i].m_energy, peak.m_energy - ClimaxMargin);
            f.m_section[i].m_energyTo = std::min(f.m_section[i].m_energyTo, peak.m_energy - ClimaxMargin);
        }
    }
    // A buildup starts where the section before it ended and arrives at the drop's energy (just under it:
    // the drop itself is the arrival), so that everything the energy drives -- the gain, the filter arcs --
    // really rises through it (Solberg and Dibben 2019: the rising middle of the U). Goa's breakdown rises
    // into the big buildup as well: the rule's 64 bars of building spirals.
    for (int i = 0; i < f.m_count; ++i) {
        Section& sec = f.m_section[i];
        if (sec.m_type == SectionTypeE::BREAK && sec.m_spiral && i + 1 < f.m_count) sec.m_energyTo = std::max(sec.m_energy, 0.5f * (sec.m_energy + f.m_section[i + 1].m_energy + 0.2f));
        if (sec.m_type != SectionTypeE::BUILD) continue;
        sec.m_energy = i > 0 ? f.m_section[i - 1].m_energyTo : 0.5f;
        sec.m_energyTo = i + 1 < f.m_count ? f.m_section[i + 1].m_energy - (f.m_section[i + 1].m_climax ? ClimaxMargin : 0.0f) : 1.0f;
        sec.m_energyTo = std::max(sec.m_energy, sec.m_energyTo);
    }
    // The mode of each section, over the tonic pedal (Form.h). It is drawn after the energies,
    // because the energy is what decides how far a section may reach for the Hijaz colour, and from
    // the section's own seed, so that locking or rerolling a section moves its mode with it. The
    // intro and the outro always keep the track's mode: they are the DJ-friendly ends of the track
    // and the next track's intro is written over them (PLAN 6.7).
    trackScale = std::clamp(trackScale, 0, NumScales - 1);
    f.m_scaleMask = 1u << trackScale;
    for (int i = 0; i < f.m_count; ++i) {
        Section& sec = f.m_section[i];
        const bool ends = sec.m_type == SectionTypeE::INTRO || sec.m_type == SectionTypeE::OUTRO;
        const uint64_t ss = sectionSeed != nullptr ? sectionSeed[std::clamp(i, 0, MaxSections - 1)]
                                                   : mixSeed(seed ^ SaltMode, static_cast<uint64_t>(i));
        sec.m_scale = ends ? trackScale
                         : borrowedMode(s, ss, trackScale, 0.5f * (sec.m_energy + sec.m_energyTo));
        f.m_scaleMask |= 1u << std::clamp(sec.m_scale, 0, NumScales - 1);
    }
    return f;
}

int sectionOfBar(const FormPlan& f, int barInTrack)
{
    for (int i = f.m_count - 1; i >= 0; --i) if (barInTrack >= f.m_section[i].m_startBar) return i;
    return 0;
}

float sectionTrimDb(const FormPlan& f, int index, double u)
{
    if (index < 0 || index >= f.m_count) return 0.0f;
    const Section& s = f.m_section[index];
    if (s.m_type == SectionTypeE::DROP && !s.m_climax) return -Drop1HoldDb;
    if (s.m_type == SectionTypeE::INTRO) return -IntroTrimDb;
    if (s.m_type == SectionTypeE::BREAK) return -BreakTrimDb;
    if (s.m_type == SectionTypeE::BUILD && index + 1 < f.m_count && f.m_section[index + 1].m_climax)
        return -BuildHeadroomDb * static_cast<float>(std::clamp(u, 0.0, 1.0));
    return 0.0f;
}

/** @brief planBar, with one level of look-ahead for the parts of the following bar. */
static BarPlan planBarImpl(const FormPlan& f, const PartAvailability& a, const uint64_t* sectionSeed, int barInTrack, bool lookAhead)
{
    BarPlan bp;
    const int si = sectionOfBar(f, barInTrack);
    const Section& s = f.m_section[si];
    const int b = std::max(0, barInTrack - s.m_startBar);
    bp.m_index = si;
    bp.m_type = s.m_type;
    bp.m_barInSection = b;
    bp.m_group = barInTrack / 8;
    const double u = s.m_bars > 1 ? static_cast<double>(b) / (s.m_bars - 1) : 0.0;
    bp.m_energy = static_cast<float>(s.m_energy + (s.m_energyTo - s.m_energy) * u);
    bp.m_pdbVariant = s.m_pdbVariant;
    bp.m_scale = static_cast<int8_t>(s.m_scale);   // the melodic layer's mode here; the bass ignores it

    // Section-level draws, in a fixed order, so the same bar always gets the same answer.
    const uint64_t ss = sectionSeed[std::clamp(si, 0, MaxSections - 1)];
    Rng rs;
    rs.seed(mixSeed(ss ^ SaltSection, 0));
    // Drawn and no longer read (19.09.2026: the kick enters on bar 17 of every intro, IntroKickBar), but
    // still drawn, so that every later draw of the section keeps its place in the stream.
    (void)rs.below(5);
    // A drop brings everything back at once (the instrumentation matrix of PLAN 6.1), which is also
    // what makes Solberg and Dibben's Track 2 rule hold: after the drop the spectrum must be at least
    // as full as it was before the break. Every other section may leave a voice out.
    const bool drop = s.m_type == SectionTypeE::DROP;
    // Every line's share of the sections is its knob (the lead since 21.09.2026, acid and arp since
    // 22.09.2026). Acid and arp kept a fixed draw here only because they were still thinned once per
    // *track*, and a second thinning would have emptied them twice; that track-level draw is gone
    // (Melody.cpp, makeMelodyPlan), so the thinning happens here alone. The share is about what it
    // was -- acid 0.9 x 0.6 = 0.54 of the sections before, 0.6 now; arp 0.8 x 0.5 = 0.40 against
    // 0.5 -- but it is a share of the sections instead of of the whole night, so a track no longer
    // simply lacks its acid line.
    const bool drawAcid = rs.uniform() < a.m_amount[mpIndex(MelodyPartE::ACID)],
               drawLead = rs.uniform() < a.m_amount[mpIndex(MelodyPartE::LEAD)],
               drawArp = rs.uniform() < a.m_amount[mpIndex(MelodyPartE::ARP)];
    const bool useAcid = a.m_part[mpIndex(MelodyPartE::ACID)] && (drop || drawAcid);
    const bool useLead = a.m_part[mpIndex(MelodyPartE::LEAD)] && (drop || drawLead);
    const bool useArp = a.m_part[mpIndex(MelodyPartE::ARP)] && (drop || drawArp);
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
    const bool useCounter = a.m_part[mpIndex(MelodyPartE::COUNTER)] && (s.m_climax || s.m_type == SectionTypeE::BREAK || rs.uniform() < 0.75f);
    const bool useStab = a.m_part[mpIndex(MelodyPartE::STAB)] && (rs.uniform() < (drop ? 0.75f : 0.55f) || s.m_dry);
    const int voiceParity = rs.below(2);            // which eight-bar groups the counter takes; the stab takes the others
    const bool droneCore = a.m_part[mpIndex(MelodyPartE::DRONE)] && rs.uniform() < 0.5f;
    const uint8_t bAcid = partBit(MelodyPartE::ACID), bLead = partBit(MelodyPartE::LEAD), bCounter = partBit(MelodyPartE::COUNTER),
                  bArp = partBit(MelodyPartE::ARP), bStab = partBit(MelodyPartE::STAB), bPad = partBit(MelodyPartE::PAD),
                  bDrone = partBit(MelodyPartE::DRONE);
    const bool hasPad = a.m_part[mpIndex(MelodyPartE::PAD)], hasDrone = a.m_part[mpIndex(MelodyPartE::DRONE)];

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
        rr.seed(mixSeed(ss ^ SaltGroup, static_cast<uint64_t>(g)));
        int draw = rr.below(4);
        if (g > 0 && draw == figure) draw = (draw + 1) % 4;
        figure = draw;
    }

    const int maxLayers = std::max(1, a.m_percLayers);
    const int hatLayers = std::clamp(a.m_hatLayers, 1, maxLayers);
    uint8_t parts = 0;
    int layers = maxLayers;
    // A bar that plays nothing but what the rule names: the outro's kick, bass and hat, the last bar of a
    // pre-drop break. No pad, no drone, no voice is added to it below.
    bool bare = false;
    bp.m_cycleBar = b % 32;

    // 19.09.2026, round "arrangement": the instrumentation of the user's two-drop form. Bars are 0-based
    // here; the comments give the rule's 1-based bars of a 256-bar Full-On track.
    switch (s.m_type) {
    case SectionTypeE::INTRO: {
        // Bars 1-16 without a kick: pads and textures, a quiet closed hat on the sixteenths, the shaker
        // from bar 9 (one change inside the first sixteen). Bars 17-32: kick and bass, the off-beat hat.
        // Inside a set (23.09.2026, round "DJ") the kick enters at the hand-over instead -- the bass swap
        // of the blend (Form.h, djOverlapBars): the outgoing kick and bass hold the floor until then, and
        // over the blend's second half the incoming percussion builds up under them, one layer at a time.
        const int kickBar = f.m_handover > 0 ? f.m_handover : IntroKickBar;
        const bool kick = b >= kickBar;
        bp.m_kickBeats = kick ? 0xF : 0;
        bp.m_bassBeats = kick ? 0xF : 0;
        // The build-up counts *down to* the kick bar, so the last eight bars before the swap carry the
        // hat layers the groove then adds one to (the groove's rule: one layer more every eight bars).
        layers = kick ? hatLayers : (f.m_handover > 0 && b >= kickBar / 2 ? std::max(1, hatLayers - (kickBar - 1 - b) / 8) : 0);
        bp.m_quietHats = !kick;
        bp.m_hatLevel = kick ? 1.0f : 0.45f;
        bp.m_offbeatHat = kick;
        bp.m_shaker = b >= 8;
        if (hasPad) parts |= bPad;
        // Fills only once the kick is in: bar 24 and bar 32 of the intro close their groups.
        bp.m_fills = kick;
        break;
    }
    case SectionTypeE::GROOVE:
        // The first core: kick and bass at full pressure, the acid as the first rhythmic lead, and one
        // percussion layer more every eight bars -- the clap on 2 and 4, then the congas, then the ride
        // (Rhythm.cpp puts them in that order behind the hats). The lead waits for drop 1.
        layers = std::min(maxLayers, hatLayers + 1 + groupInSection);
        bp.m_shaker = true;                     // the intro's shaker stays: layers join here, none leaves
        if (a.m_part[mpIndex(MelodyPartE::ACID)]) parts |= bAcid;
        else if (useArp) parts |= bArp;       // no acid in this track: the arp is the rhythmic lead
        break;
    case SectionTypeE::BUILD: {
        // Layers return one per four bars, the hats close up to sixteenths for the second half, the roll
        // runs over the last rollBars bars. Buildup 1 thins the bass (beats 1 and 3) for the four bars
        // before its pre-drop break; the break itself has neither kick nor bass, and its last bar stops
        // on beat 4 -- that one beat is left to a vocal or a single zap (makeFormSfx).
        const int rollStart = s.m_bars - std::max(0, s.m_rollBars);
        const int pdbStart = s.m_bars - std::max(0, s.m_pdbBars);
        layers = std::min(maxLayers, 1 + b / 4);
        if (useAcid) parts |= bAcid;
        if (useArp && b >= s.m_bars / 2) parts |= bArp;
        bp.m_hatsDense = b >= s.m_bars / 2;
        if (s.m_rollBars > 0 && b >= rollStart) { bp.m_rollBar = b - rollStart; bp.m_rollBars = s.m_rollBars; }
        // The kick's own roll (23.09.2026, round "Set-Kurve"; BarPlan::kickRoll). The literature's riser has
        // three layers -- the tonal sweep, the noise and a *rhythmic* one -- and the snare carried the third
        // alone. In the bar before the pre-drop break of a buildup with a long roll (the big buildup: its roll
        // is eight bars or more and its break one bar) the kick doubles to eighths, and in Full-On and Hi-Tech
        // half of those times to sixteenths over the second half of the bar, the last acceleration before the
        // vacuum. Per form template, because that is what the section knows of its style: Full-On/Hi-Tech
        // 0.55, Progressive 0.20, Goa 0.35, Dark Forest 0.30 of the tracks. Its own stream, after every older
        // draw of the section.
        if (s.m_rollBars >= 8 && s.m_pdbBars == 1 && b == s.m_bars - 2) {
            static const float KickRollChance[NumBodies] = { 0.55f, 0.20f, 0.35f, 0.30f };
            Rng kr;
            kr.seed(mixSeed(ss ^ SaltKickRoll, static_cast<uint64_t>(b)));
            const bool roll = kr.uniform() < KickRollChance[std::clamp(f.m_body, 0, NumBodies - 1)];
            const bool fast = kr.uniform() < 0.5f;
            if (roll) bp.m_kickRoll = static_cast<int8_t>(f.m_body == 0 && fast ? 2 : 1);
        }
        if (s.m_pdbBars >= 4 && b >= pdbStart - 4 && b < pdbStart) bp.m_bassBeats = 0x5;
        if (s.m_pdbBars > 1 && b >= pdbStart && b < s.m_bars - 1) {
            // The pre-drop break before its last bar: kick and bass out, the roll and the hats go on.
            // BarPlan::pdb stays for the last bar alone, whose roll stops on beat 3.
            bp.m_kickBeats = 0;
            bp.m_bassBeats = 0;
            layers = std::min(layers, hatLayers);
        }
        if (s.m_pdbBars > 0 && b == s.m_bars - 1) {
            // The last bar before the drop. Variants: whole bar, half bar, beat 4 only; the fourth (the
            // kick alone on beat 4) is never drawn since 19.09.2026, and a longer pre-drop break has had
            // no kick for bars already, so there the whole bar is empty but for the roll.
            bp.m_pdb = true;
            static const uint8_t KickMask[NumPdbVariants] = { 0x0, 0x3, 0x7, 0x7 };
            static const uint8_t BassMask[NumPdbVariants] = { 0x0, 0x3, 0x7, 0x7 };
            const int v = s.m_pdbBars > 1 ? 0 : std::clamp(s.m_pdbVariant, 0, NumPdbVariants - 1);
            bp.m_kickBeats = KickMask[v];
            bp.m_bassBeats = BassMask[v];
            parts = 0;
            layers = 0;
            bare = true;
        }
        break;
    }
    case SectionTypeE::DROP: {
        // Everything back at once. Inside every 32-bar cycle the first sixteen bars keep one layer back
        // and bar 17 brings it in with a crash on the one -- the micro rule's "bar 16: a crash on the one
        // and a new percussion element"; the drop's own downbeat has its crash too.
        const int cycleGroup = (b % 32) / 8;
        // Drop 1 holds one layer more back than drop 2 would (19.09.2026, round "polish": the climax needs
        // room above it; Drop1HoldDb), where the kit is big enough to still add one on bar 17.
        const int hold = !s.m_climax && maxLayers > 3 ? 1 : 0;
        layers = maxLayers - hold - (cycleGroup < 2 && maxLayers > 2 ? 1 : 0);
        bp.m_crash = b % 16 == 0;
        bp.m_shaker = true;
        if (useAcid) parts |= bAcid;
        if (useLead) parts |= bLead;
        if (useArp) parts |= bArp;
        if (s.m_climax) { bp.m_openHats = true; bp.m_ride = true; layers = maxLayers; }
        // The Abriss before a sixteen-bar line (23.09.2026, round "Set-Kurve"): in the bar before a crash bar
        // -- never the drop's last, whose end the next section's cut owns -- kick and bass leave beat 4, so
        // that the crash on the one lands into a hole, the DJ's tear before the phrase. The fill of that bar
        // (Rhythm.cpp, chooseFill: the snare roll at bar 16) plays over the hole. Per form template: Full-On/
        // Hi-Tech 0.45, Progressive 0.15, Goa 0.30, Dark Forest 0.25 of these bars; its own stream.
        if (b % 16 == 15 && b + 1 < s.m_bars) {
            static const float AbrissChance[NumBodies] = { 0.45f, 0.15f, 0.30f, 0.25f };
            Rng ab;
            ab.seed(mixSeed(ss ^ SaltAbriss, static_cast<uint64_t>(b)));
            if (ab.uniform() < AbrissChance[std::clamp(f.m_body, 0, NumBodies - 1)]) { bp.m_kickBeats = 0x7; bp.m_bassBeats = 0x7; }
        }
        break;
    }
    case SectionTypeE::BREAK: {
        // Solberg and Dibben 2019: the sudden removal of bass and bass drum -- the rule's bass vacuum. Pads
        // carry it, with one thinned melodic voice; after sixteen bars a quiet off-beat hat returns to keep
        // the tempo in the dancer's head. Goa: the arp spirals through the whole breakdown. Dark Forest:
        // two layers of percussion keep going.
        bp.m_kickBeats = 0;
        bp.m_bassBeats = 0;
        const int hatBack = std::min(16, s.m_bars / 2);
        layers = std::min(maxLayers, s.m_breakPerc);
        bp.m_offbeatHat = b >= hatBack;
        bp.m_hatLevel = 0.5f;
        if (hasPad) parts |= bPad;
        // The user's Model 3 (20.09.2026, round "dialogue"): drop 1 is the lead alone, the **main
        // breakdown is the counter-lead alone, introduced without the lead**, and drop 2 is the two of
        // them interlocking. The middle step is what makes drop 2 land: without it the counter plays in
        // drop 2 only, and a listener meets it for the first time already interlocked with the lead. A breakdown that carries the counter therefore never carries the
        // lead -- it is an introduction, and an introduction has nothing beside it.
        //
        // Per style, through the section's own fields rather than a style index: Goa spirals its arp
        // through the whole breakdown (s.spiral), so there the counter enters at the half, over the
        // spiral; every other style has the bare pad carpet and the counter enters at the quarter --
        // in Dark Forest's sixteen-bar breakdown that is bar 5, in a 64-bar one bar 17.
        const bool counterBreak = useCounter && a.m_part[mpIndex(MelodyPartE::COUNTER)];
        const int counterFrom = s.m_spiral ? s.m_bars / 2 : s.m_bars / 4;
        if (s.m_spiral && a.m_part[mpIndex(MelodyPartE::ARP)]) parts |= bArp;
        else if (!counterBreak && b >= s.m_bars / 4) parts |= breakLead ? (useLead ? bLead : 0) : (useArp ? bArp : 0);
        if (counterBreak && b >= counterFrom) parts |= bCounter;
        bp.m_fills = false;
        if (b == 0) bp.m_cutBeats = s.m_cutBeats;
        break;
    }
    case SectionTypeE::OUTRO: {
        // The inverse of the intro: a layer leaves every eight bars -- the leads at once, then the acid and
        // two percussion layers, then the pads and the rest. Since 23.09.2026 (round "DJ") the outro's last
        // `overlapTail` bars are the *blend*: the next track's intro sounds over them and owns the
        // polyphonic voices, so this track keeps its floor -- kick, bass, the acid over the blend's first
        // half, the percussion thinning by a layer every eight bars -- and nothing polyphonic; the last
        // eight bars are kick, bass and one hat, the outgoing groove alone under the incoming atmosphere.
        const int blendFrom = s.m_bars - std::clamp(f.m_overlapTail, OutroBareBars, s.m_bars);
        const int bareFrom = s.m_bars - std::min(OutroBareBars, s.m_bars);
        if (b >= bareFrom) {
            layers = 0;
            bp.m_offbeatHat = true;
            bare = true;
        } else if (b >= blendFrom) {
            const int blendBars = bareFrom - blendFrom, inBlend = b - blendFrom;
            const int steps = std::max(1, blendBars / 8);
            layers = std::max(1, hatLayers - (hatLayers - 1) * (inBlend / 8) / steps);
            bp.m_offbeatHat = true;
            if (useAcid && inBlend < blendBars / 2) parts |= bAcid;
            bare = true;   // no pad, no drone, no voice: the incoming track's own sound there
        } else {
            // Something leaves at every step: two layers, or -- where the kit has too few for that -- all
            // but the one hat of the bare end.
            const int first = std::max(hatLayers, maxLayers - 1);
            const int step = b / 8;
            layers = step == 0 ? first : (first > hatLayers ? std::max(hatLayers, first - 2) : 0);
            bp.m_offbeatHat = layers == 0;
            if (useAcid && step == 0) parts |= bAcid;
            if (hasPad) parts |= bPad;
        }
        bp.m_fills = false;
        break;
    }
    default: break;
    }
    bp.m_percLayers = std::clamp(layers, 0, maxLayers);

    // Pads carry what has neither lead nor acid, and join some of the rest.
    if (hasPad && !bare && s.m_type != SectionTypeE::BUILD && s.m_type != SectionTypeE::INTRO) {
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
    const bool carriesMusic = !bare && !bp.m_pdb && (s.m_type == SectionTypeE::GROOVE || s.m_type == SectionTypeE::DROP);
    if (carriesMusic && (parts & (bAcid | bLead | bArp | bPad | bStab)) == 0) {
        const MelodyPartE Floor[4] = { MelodyPartE::PAD, MelodyPartE::ARP, MelodyPartE::LEAD, MelodyPartE::ACID };
        for (MelodyPartE part : Floor) {
            if (a.m_part[mpIndex(part)]) { parts |= partBit(part); break; }
        }
    }

    bp.m_padGate = (parts & bPad) != 0 && (parts & (bAcid | bLead)) != 0 && gate;

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
    const bool core = isCore(s.m_type);
    const bool counterGroup = s.m_climax ? groupInSection >= 1 : (groupInSection >= 1 && (groupInSection + voiceParity) % 2 == 1);
    if (s.m_climax && useCounter && (parts & bLead) != 0 && counterGroup) parts |= bCounter;
    const bool stabHere = s.m_dry ? !(s.m_climax && counterGroup && (parts & bCounter) != 0) : !counterGroup;
    if (core && useStab && stabHere && (s.m_type == SectionTypeE::DROP || b >= 8)) parts |= bStab;
    // The previous track's kick and bass still sound over the first bars of this one (the DJ overlap):
    // there the floor is theirs.
    // A buildup's pre-drop break is a held breath, not a floor to lay a sub under.
    bp.m_floorSilent = bp.m_kickBeats == 0 && bp.m_bassBeats == 0 && !bp.m_pdb && s.m_type != SectionTypeE::BUILD
                  && barInTrack >= f.m_handover;
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
    // docs/rounds/2026-09.md has the third-octave numbers.
    // 22.09.2026: what changed is only *which tracks have one*. The drone used to be drawn once per
    // track (0.6 at the knob's default), so 40 % of the night had none at all -- the user's
    // "ebensowenig wie eine Drone". Every track has one now (Melody.cpp, makeMelodyPlan). Where it
    // plays is untouched: under every bar, as the carpet argument above decided on 20.09.2026. A
    // first attempt (22.09.2026) thinned it per section with the knob as the probability, and that
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
    const int leadOct = (s.m_energy < 0.45f && a.m_leadLo >= 71) ? -1 : 0;
    bp.m_arpOctave = static_cast<int8_t>(s.m_climax ? 1 : 0);
    bp.m_leadOctave = static_cast<int8_t>(leadOct);
    bp.m_climax = s.m_climax;
    bp.m_sectionBars = s.m_bars;
    // The main breakdown (22.09.2026): the breakdown whose next drop is the climax. It is the one
    // place the pad's harmony leaves the pendulum -- the aeolian three, or one chord held throughout.
    if (s.m_type == SectionTypeE::BREAK) {
        for (int j = si + 1; j < f.m_count; ++j) {
            if (f.m_section[j].m_type != SectionTypeE::DROP) continue;
            bp.m_mainBreak = f.m_section[j].m_climax;
            break;
        }
    }
    // Behind the main breakdown (23.09.2026, round "Harmonie"): the pad may change its progression there.
    for (int k = 0; k < si; ++k) {
        if (f.m_section[k].m_type != SectionTypeE::BREAK) continue;
        for (int j = k + 1; j < f.m_count; ++j) {
            if (f.m_section[j].m_type != SectionTypeE::DROP) continue;
            if (f.m_section[j].m_climax) bp.m_afterMainBreak = true;
            break;
        }
    }
    bp.m_parts = parts;

    // The group's figure: at beat 1 of the last bar of every eight-bar group in which the bass plays --
    // the cores as before, and since 19.09.2026 the intro after the kick and the outro, whose last
    // sixteen bars would otherwise be two identical groups of kick, bass and hat. The figures differ in
    // their last note for every bass pattern, so no two consecutive groups are identical.
    const bool figureSection = core || s.m_type == SectionTypeE::INTRO || s.m_type == SectionTypeE::OUTRO;
    if (figureSection && b % 8 == 7 && (bp.m_bassBeats & 0x2) != 0) bp.m_groupFigure = GroupFigures[figure];
    // What the next bar plays, so that an acid slide at the end of a bar knows whether there is still
    // a note to slide into (the last bar of an acid section must not slide into silence).
    if (lookAhead && barInTrack + 1 < f.m_bars) bp.m_partsNext = planBarImpl(f, a, sectionSeed, barInTrack + 1, false).m_parts;
    return bp;
}

BarPlan planBar(const FormPlan& f, const PartAvailability& a, const uint64_t* sectionSeed, int barInTrack)
{
    return planBarImpl(f, a, sectionSeed, barInTrack, true);
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
 * section's seed (0.75 .. 1 of RideCutoff). Decay and resonance are written as their own strands,
 * relative to the knobs: the decay as ratios of the knob (RideDecayShort, RideDecayLong), the
 * resonance to "at most the knob, 0.55" and "at least the knob, 0.85". One consequence to know: these
 * strands replace, for the rest of the section, the track's recipe offset on decay and resonance that
 * Composer writes at the track's start -- a control strand holds one offset, not a sum.
 * A breakdown does what it did: one dive down over the first half and a return over the second.
 *
 * **Why it cannot fight the accent sweep.** The 303's accent charges a capacitor with SweepTau =
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
 * buildup the percussion's hall send climbs to RollSend and is cut back to nothing at the section
 * that follows, which is the drop. Nothing else is needed for the cut -- the pre-drop break already
 * empties beat 4 (Form.h) -- and a send that is still open into the drop would smear exactly the
 * transient the whole gesture exists to sharpen.
 */
void sectionAutomation(const ParamStore& p, const Section& s, uint64_t seed, double beat,
                       float base0, float base1, bool knobs, std::vector<ControlEvent>& out, float resoBase, float decayBase)
{
    const int ab = p.base(ModuleE::ACID), mb = p.base(ModuleE::MIX);
    // The strands below are written one after the other; the events are put into time order at the
    // end (stable, so two events of one parameter at the same beat keep their order), which is the
    // order every consumer of a control stream expects.
    const size_t first = out.size();
    struct Sorter {
        std::vector<ControlEvent>& m_v;
        size_t m_from;
        ~Sorter() { std::stable_sort(m_v.begin() + static_cast<std::ptrdiff_t>(m_from), m_v.end(),
                                     [](const ControlEvent& a, const ControlEvent& b) { return a.m_beat < b.m_beat; }); }
    } sorter{ out, first };
    auto push = [&](int id, float value, double at, float len) {
        ControlEvent c;
        c.m_beat = at;
        c.m_param = static_cast<int16_t>(id);
        c.m_kind = ControlEvent::KindE::OFFSET;
        c.m_value = value;
        c.m_length = len;
        out.push_back(c);
    };
    // The percussion's hall send: down at the start of every section, up over the last four bars of a
    // buildup. Writing the zero unconditionally is what makes the cut at the drop exact -- the drop is
    // a section like any other and its first event closes the send.
    push(mb + mix::PERC_HALL, 0.0f, beat, 0.0f);
    const double bars = static_cast<double>(s.m_bars);
    if (s.m_type == SectionTypeE::BUILD && !knobs) {
        // Over the roll (19.09.2026: the section's own roll, sixteen bars in the big buildup).
        const double rollBars = std::min(static_cast<double>(s.m_rollBars > 0 ? s.m_rollBars : RollBars), bars);
        push(mb + mix::PERC_HALL, RollSend, beat + (bars - rollBars) * BeatsPerBar,
             static_cast<float>(rollBars * BeatsPerBar));
    }
    if (knobs) return;

    // The modulation effects' ride (19.09.2026, round "fx-psychedelia"; PsyFx.h): the section half of
    // their automation, the event half being the engine's (Engine.h). Per section type, the depth drawn
    // from the section's own seed: a drop opens the flanger on the SFX bus, a breakdown the phaser and
    // a slow drift of the shifter, a buildup drags the shifter up over its whole length (with its riser
    // on top, the event motion), and the drop snaps it back to zero on its downbeat.
    {
        const int xb = p.base(ModuleE::PSY_FX);
        Rng fx;
        fx.seed(mixSeed(seed ^ SaltFxRide, 0));
        const float a = fx.uniform(), b = fx.uniform(), sign = fx.below(2) == 0 ? 1.0f : -1.0f;
        const float perHz = 1.0f / (p.desc(xb + psyfx::SHIFT_HZ).m_maxValue - p.desc(xb + psyfx::SHIFT_HZ).m_minValue);
        const float twoBars = static_cast<float>(2.0 * BeatsPerBar);
        float flange = 0.0f, phase = 0.0f, shiftHz = 0.0f, shiftLen = twoBars;
        switch (s.m_type) {
        case SectionTypeE::DROP:   flange = 0.15f + 0.20f * a; shiftLen = 0.0f; break;
        case SectionTypeE::GROOVE: flange = 0.10f * a; phase = 0.10f * b; break;
        case SectionTypeE::BREAK:  phase = 0.25f + 0.20f * a; shiftHz = sign * (10.0f + 30.0f * b); shiftLen = static_cast<float>(bars * BeatsPerBar); break;
        case SectionTypeE::BUILD:  flange = 0.15f * a; shiftHz = 60.0f + 60.0f * b; shiftLen = static_cast<float>(bars * BeatsPerBar); break;
        default:                  phase = 0.15f * a; shiftHz = sign * 8.0f * b; break;
        }
        push(xb + psyfx::FLANGER_MIX, flange, beat, twoBars);
        push(xb + psyfx::PHASER_MIX, phase, beat, twoBars);
        if (shiftLen > 0.0f && (s.m_type == SectionTypeE::BUILD || s.m_type == SectionTypeE::BREAK))
            push(xb + psyfx::SHIFT_HZ, 0.0f, beat, 0.0f);   // from zero, so the ramp is the section's own
        push(xb + psyfx::SHIFT_HZ, shiftHz * perHz, beat, shiftLen);
    }

    // The acid's ride. Sections shorter than four bars have nothing to ride on.
    if (bars < 4.0) return;
    Rng r;
    r.seed(mixSeed(seed ^ SaltRide, 0));
    // Where the straight line of the section's own arc stands at bar b, so the ride is an excursion
    // from it and not a replacement for it.
    auto base = [&](double b) { return base0 + (base1 - base0) * static_cast<float>(b / bars); };
    const float depth = 0.75f + 0.25f * r.uniform();   ///< how hard this section is ridden
    if (s.m_type == SectionTypeE::BREAK) {
        // The dive: down over the first half, back over the second. A breakdown is where a psytrance
        // track closes the filter, and the return is what makes the following section open.
        const double half = bars / 2.0;
        const float len = static_cast<float>(half * BeatsPerBar);
        push(ab + acid::CUTOFF, base(half) - depth * RideDive, beat, len);
        push(ab + acid::RESONANCE, resoBase - depth * RideReso, beat, len);
        push(ab + acid::CUTOFF, base1, beat + half * BeatsPerBar, len);
        push(ab + acid::RESONANCE, resoBase, beat + half * BeatsPerBar, len);
        return;
    }
    // Everything else rides the four-stage cycle (Form.h, RideShape): 32 bars, or the whole section
    // when it is shorter, so that a buildup's cycle ends in the dive exactly on the drop.
    const RideShape shape = acidRideShape(bars);
    // 19.09.2026 (round "voices"): the ride is an excursion around the *voiced* resonance and decay --
    // the knob plus the track's acid voicing (resoBase, decayBase) -- instead of around the knob, so a
    // track's voicing can own the two quantities the section rides. With both bases at zero (the first
    // track, which plays the knobs) every event below is the one it was.
    const int resoId = ab + acid::RESONANCE, decayId = ab + acid::DECAY;
    const double knobReso = p.get(resoId);
    const double voicedReso = p.fromNormalised(resoId, p.toNormalised(resoId, p.get(resoId)) + resoBase);
    const double knobDecay = p.get(decayId);
    const double voicedDecay = p.fromNormalised(decayId, p.toNormalised(decayId, p.get(decayId)) + decayBase);
    const ParamDesc& dd = p.desc(decayId);
    // Decay is a log parameter: a ratio of the voiced decay is a fixed normalised distance from it.
    const double decaySpan = std::log(static_cast<double>(dd.m_maxValue) / dd.m_minValue);
    auto decayOffset = [&](double ratio) {
        const double target = std::clamp(voicedDecay * ratio, static_cast<double>(dd.m_minValue), static_cast<double>(dd.m_maxValue));
        return static_cast<float>(std::log(target / knobDecay) / decaySpan);
    };
    const float resoMedium = static_cast<float>(std::min(voicedReso, static_cast<double>(RideResoMedium)) - knobReso);
    const float resoSquelch = static_cast<float>(std::max(voicedReso, static_cast<double>(RideResoSquelch)) - knobReso);
    const float h = 0.5f * depth * RideCutoff;   ///< half the peak-to-peak cutoff excursion
    for (double c0 = 0.0; c0 + shape.m_length <= bars + 1e-9; c0 += shape.m_length) {
        const double at0 = beat + c0 * BeatsPerBar;
        auto at = [&](double barInCycle) { return at0 + barInCycle * BeatsPerBar; };
        auto len = [&](double barsLong) { return static_cast<float>(barsLong * BeatsPerBar); };
        auto cut = [&](int k) { return base(c0 + shape.m_bar[k]) + h * shape.m_cutoff[k]; };
        // Cutoff: into "almost closed" over the first bar, hold, open over stages 2 to 4, dive.
        push(ab + acid::CUTOFF, cut(0), at(0.0), len(1.0));
        for (int k = 1; k < RideShape::Points; ++k)
            push(ab + acid::CUTOFF, cut(k), at(shape.m_bar[k - 1] + shape.m_hold[k - 1]),
                 len(shape.m_bar[k] - shape.m_bar[k - 1] - shape.m_hold[k - 1]));
        // The micro rule's bar 24 (19.09.2026): a short sweep on the acid -- up by RideSweep half-
        // excursions over the first half of the cycle's 24th bar and back onto the ride's line over the
        // second, arriving where the ride itself arrives at the end of stage 3. Only in full 32-bar cycles.
        if (shape.m_length >= 32.0) {
            const double b24 = shape.m_stage[3] - 1.0;
            push(ab + acid::CUTOFF, base(c0 + b24 + 0.5) + h * (shape.m_cutoff[3] + RideSweep), at(b24), len(0.5));
            push(ab + acid::CUTOFF, cut(3), at(b24 + 0.5), len(0.5));
        }
        // Resonance: medium through stages 1 and 2, up to the squelch over stage 3, held to the end.
        push(ab + acid::RESONANCE, resoMedium, at(0.0), len(1.0));
        push(ab + acid::RESONANCE, resoSquelch, at(shape.m_stage[2]), len(shape.m_stage[3] - shape.m_stage[2]));
        // Decay: short and dry in stage 1, back to the knob over stage 2, longer through 3 and 4.
        push(ab + acid::DECAY, decayOffset(RideDecayShort), at(0.0), len(1.0));
        push(ab + acid::DECAY, decayOffset(1.0), at(shape.m_stage[1]), len(shape.m_stage[2] - shape.m_stage[1]));
        push(ab + acid::DECAY, decayOffset(RideDecayLong), at(shape.m_stage[2]), len(shape.m_stage[3] + (shape.m_length - shape.m_stage[3]) * 0.5 - shape.m_stage[2]));
    }
}

RideShape acidRideShape(double sectionBars)
{
    RideShape s;
    s.m_length = sectionBars >= 32.0 ? 32.0 : sectionBars;
    const double L = s.m_length;
    for (int k = 0; k < 4; ++k) s.m_stage[k] = L * k / 4.0;
    // The dive takes the last bar (the last two of a 32-bar cycle would eat a quarter of stage 4);
    // the full opening is reached halfway through stage 4 and held until the dive.
    const double dive = 1.0;
    // Key points of the cutoff, in units of half the excursion before centring, and when each is
    // reached: closed through stage 1, a little open at the end of stage 2, well open at the end of
    // stage 3, fully open halfway into stage 4, closed again at the end of the dive.
    const double raw[RideShape::Points] = { -1.0, -1.0, -0.2, 0.5, 1.0, 1.0, -1.0 };
    const double when[RideShape::Points] = { 0.0, L / 4.0, L / 2.0, 3.0 * L / 4.0, 7.0 * L / 8.0, L - dive, L };
    // The cycle's mean, with raised-cosine ramps between the points (a raised cosine between a and b
    // averages to (a + b) / 2) -- subtracted, so the ride swings around the section's own line and
    // the level match, which measures the knobs, stays honest (Form.h).
    double area = 0.0;
    for (int k = 1; k < RideShape::Points; ++k) area += 0.5 * (raw[k] + raw[k - 1]) * (when[k] - when[k - 1]);
    const double mean = area / L;
    for (int k = 0; k < RideShape::Points; ++k) {
        s.m_cutoff[k] = static_cast<float>(raw[k] - mean);
        s.m_bar[k] = when[k];
        s.m_hold[k] = 0.0;
    }
    // Point 0 is reached over the first bar (from wherever the previous section left the filter) and
    // held to point 1; the events of point k start where point k-1 was reached plus its hold.
    s.m_hold[0] = 1.0;
    s.m_meanRaw = static_cast<float>(mean);
    return s;
}

} // namespace phos
