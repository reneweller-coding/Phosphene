/**
 * @file FormSfx.cpp
 * @brief Where a track's effects go: the transition markers, the ear candy, the psychedelic layer, the voices and
 *        the shamanic bed (the form itself is Form.cpp's).
 */
#include "phos/Form.h"
#include "phos/Preferences.h"
#include "phos/Dsp.h"
#include "phos/Sfx.h"
#include "phos/Util.h"
#include <algorithm>
#include <cmath>

#include "Salts.h"
using namespace phos::salts::form;   // the form's seed salts (Salts.h)

namespace phos {

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
            // With its sub drop, as every impact has one (the pass above ran before this impact existed); the kick's
            // ducker keeps it off the incoming kick's transient (Engine.h).
            if (m.uniform() < pMark) { add(swap, 4.0f, SfxType::Impact); add(swap, 4.0f, SfxType::SubDrop); }
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
            // 24.09.2026, the user again: "Die Zips und Zaps kommen nach wie vor viel zu oft". Counted over twelve
            // seeds of three tracks: 1.7 short one-shots per eight bars of groove, 1.9 in a drop, 5.7 in drop 2.
            // The two-bar candy 0.5 -> 0.15, the four-bar gesture 1.2 -> 0.8: the candy lives at the phrase ends.
            const float p = amount * density * trackDensity * (four ? 0.8f : 0.15f);
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
            // 24.09.2026: still "viel zu oft" -- 0.7 a bar was 5.7 short one-shots per eight bars of drop 2. One
            // squelch in the last bar of a four-bar group, at 0.7 of sfx_amount, and no pair: about one per eight
            // bars at the default. The three draws stay per bar, so the stream below keeps its shape.
            (void)k1;
            if (b % 4 != 3 || roll >= 0.7f * amount) continue;
            add(start + b * bar + kGaps[k0], 0.25f, SfxType::Squelch);
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
    // 24.09.2026 ("nach wie vor viel zu oft"): a phrase -- seven bars may pass, the eighth is filled -- and
    // never with a short one-shot: the floor is there so a groove does not go dry, not to add zips.
    static const double kFillWeights[4] = { 0.55, 0.45, 0.0, 0.0 };   // sweep, reverse swell, zap, squelch
    static const SfxType kFillTypes[4] = { SfxType::Sweep, SfxType::ReverseSwell, SfxType::Zap, SfxType::Squelch };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        if (s.type != SectionType::Groove && s.type != SectionType::Drop) continue;
        for (int b = 0; b < s.bars; ++b) {
            const int at = s.startBar + b;
            if (at <= 2 || at >= lastBar) continue;
            bool any = false;
            for (int k = 0; k <= 7 && k <= at; ++k) any = any || carries[static_cast<size_t>(at - k)] != 0;
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

/**
 * @brief The effects of a track: a marker at every section transition, ear candy inside the long ones.
 *
 * **Transitions: every one of them, not a draw per effect.** Until 18.09.2026 each effect
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
 * @param f            the form the events are added to (FormPlan::sfx)
 * @param seed         the track's form seed; every placement draws from a salted stream of it
 */
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

void placeField(FormPlan& f, uint64_t seed, int style, float density, float amount)
{
    // How often a style has its place (StyleId order: Goa, Full-On, Progressive, Dark Forest, Hi-Tech): the forest
    // styles live in one (the user: "insbesondere für Forest"), the machine styles only visit.
    static const float kPlace[kNumStyles] = { 0.7f, 0.55f, 0.8f, 1.0f, 0.5f };
    static const float kGroove[kNumStyles] = { 0.0f, 0.0f, 0.3f, 0.8f, 0.0f };
    static const float kShot[kNumStyles] = { 0.4f, 0.6f, 0.25f, 0.15f, 0.5f };
    const int st = std::clamp(style, 0, kNumStyles - 1);
    const double bar = kBeatsPerBar;
    const float d = std::max(0.0f, density);
    Rng r;
    r.seed(mixSeed(seed ^ kSaltBed, 0x4649454C44ull));
    auto place = [&](double beat, int bars, int velocity) {
        if (bars <= 0) return;
        SfxEvent e;
        e.beat = std::max(0.0, beat);
        e.length = static_cast<float>(bars * bar);
        e.variant = static_cast<uint16_t>(std::clamp(velocity, 1, 127));
        f.field.push_back(e);
    };
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        const float u = r.uniform();
        switch (s.type) {
        case SectionType::Intro: if (u < std::min(1.0f, 0.95f * kPlace[st] * d)) place(start, s.bars, 100); break;
        case SectionType::Break: if (u < std::min(1.0f, 0.9f * kPlace[st] * d)) place(start, s.bars, 105); break;
        case SectionType::Outro: {
            const int live = s.bars - kDjOverlapMax;   // up to the blend: the next track's place takes over there
            if (live >= 8 && u < std::min(1.0f, 0.75f * kPlace[st] * d)) place(start, live, 95);
            break;
        }
        case SectionType::Groove:
            for (int b = 8; b + 16 <= s.bars; b += 32)
                if (r.uniform() < std::min(1.0f, kGroove[st] * d)) place(start + b * bar, 16, 50);   // under a groove: 6 dB under an intro's
            break;
        default: break;
        }
    }
    // The NASA shots, away from the voices.
    std::vector<double> voices;
    for (const SfxEvent& e : f.sfx)
        if (sfxTypePart(static_cast<SfxType>(std::clamp(e.type, 0, kNumSfxTypes - 1))) == Part::Vocal) voices.push_back(e.beat);
    auto free = [&](double beat) {
        for (double v : voices) if (std::fabs(v - beat) < 2.0 * bar) return false;
        return true;
    };
    const float pShot = kShot[st] * std::min(1.0f, 2.0f * std::max(0.0f, amount));
    for (int i = 0; i < f.count; ++i) {
        const Section& s = f.section[i];
        const double start = static_cast<double>(s.startBar) * bar;
        const float u = r.uniform();
        int at = -1;
        if (s.type == SectionType::Break && s.bars >= 8 && u < pShot) at = 2;
        if (s.type == SectionType::Intro && s.bars >= 16 && u < 0.5f * pShot) at = 8;
        if (at < 0) continue;
        for (int tries = 0; tries < 3 && !free(start + at * bar); ++tries) at += 4;
        if (at + 2 > s.bars || !free(start + at * bar)) continue;
        SfxEvent e;
        e.beat = start + at * bar;
        e.length = static_cast<float>(4.0 * bar);
        e.type = static_cast<int>(SfxType::SpaceShot);
        f.sfx.push_back(e);
    }
    std::stable_sort(f.sfx.begin(), f.sfx.end(), [](const SfxEvent& a, const SfxEvent& b) { return a.beat < b.beat; });
}

} // namespace phos
