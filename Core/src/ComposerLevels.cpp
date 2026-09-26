/**
 * @file ComposerLevels.cpp
 * @brief Measuring a track: the level match, presence, audibility and Auto Gain, and the probe renders they read.
 */
#include "phos/Composer.h"
#include "ComposerInternal.h"
#include "phos/Audibility.h"
#include "phos/Preferences.h"
#include "phos/Probe.h"
#include "phos/Corpus.h"
#include "phos/Disperser.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Harmony.h"
#include "phos/Loudness.h"
#include "phos/Model.h"
#include "phos/Params.h"
#include "phos/Patterns.h"
#include "phos/Sfx.h"
#include "phos/Util.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

#include "Salts.h"
using namespace phos::salts::composer;   // the Composer's seed salts (Salts.h)

namespace phos {

namespace {

constexpr double kPresenceRefDb = -7.43;   ///< the probe's reading at the reference median (calibrated, see above)
constexpr double kPresenceBandDb = 1.5;    ///< no correction within this distance of the median
constexpr double kPresenceCutDb = 6.0;     ///< the most the lines are taken down (kept at 6, not widened -- see above)
constexpr double kPresenceLiftDb = 3.0;    ///< the most they are brought up

constexpr double kAudibleLiftDb = 4.0;   ///< the most one line is lifted
struct AudibleWant { MelodyPart part; double share; };
constexpr AudibleWant kAudibleWant[] = { { MelodyPart::Counter, 0.60 }, { MelodyPart::Arp, 0.35 }, { MelodyPart::Stab, 0.35 } };

} // namespace

void Composer::matchMaster(const ParamStore& p, TrackPlan& t, const double* firstReading) const
{
    const int ms = p.base(Module::Master);
    t.masterGainDb = 0.0f;
    if (!p.getBool(ms + master::AutoGain)) return;
    const double target = p.get(ms + master::TargetLufs);
    // Measure, correct, measure again, and take a secant step between the two readings.
    // The first reading may have been rendered already, beside the presence probes (measureTrack).
    const double l0 = firstReading != nullptr ? *firstReading : probeLoudness(p, t, -2, 0.0f);
    t.mixLoudness = l0;
    if (l0 < -60.0) return;
    const double g1 = std::clamp(target - l0, -18.0, 18.0);
    const double l1 = probeLoudness(p, t, -2, static_cast<float>(g1));
    const double slope = std::fabs(g1) > 0.05 ? (l1 - l0) / g1 : 1.0;
    const double g2 = g1 + (target - l1) / std::clamp(slope, 0.2, 1.0);
    t.masterGainDb = static_cast<float>(std::clamp(g2, -18.0, 18.0));
}

/**
 * The presence match (19.09.2026, round "polish"). Measured over 30 tracks of the arrangement round (5 styles,
 * 2 seeds, 3 tracks each; Tools/metrics.py on the rendered drops): the presence band (1.5..6 kHz against the
 * kick-and-bass band 40..140 Hz) of drop 1 ran from -6.2 to +7.1 dB around the reference recordings' median,
 * interquartile -4.4 .. -0.7 -- the listening seed's track 2 at +2.8 / +3.7 over it, its track 1 at -3.1. The
 * level match cannot see it: it matches each part's *loudness* to the first track's, and a line that is
 * brighter at the same loudness puts more into the presence band. A cut on every track would push the dark
 * ones further down, so the match is per track and only outside a band around the median:
 *
 *  - two probe renders of the drops (four bars of each, one per eight-bar group, the form's own instrumentation
 *    and section controls, the master's dynamics off): the lines alone -- lead, counter, arp, stab, the voices
 *    whose recipes carry the brightness -- and everything else. Both are linear in the lines' gain g, so
 *    presence(g) = (g Pl + Pr) / (g Ll + Lr) is solved for g in closed form, no third render;
 *  - the estimate is read against the reference median through kPresenceRefDb: the probe's raw ratio minus it is
 *    the rendered drops' presence (the power mean of the two drops, Tools/metrics.py bands) over 30 calibration
 *    tracks (5 styles, 2 seeds, 3 tracks each; the match off) with a median error of -0.04 dB, interquartile
 *    -0.33 .. +0.33 dB, standard deviation 0.81, correlation 0.97; the three worst read the render 1.8 .. 2.5 dB
 *    low (bright tracks whose drop 2 carries more than the probe's four bars show). Two bars from the middle of each drop without the section controls
 *    had read the bright tracks up to 2.4 dB low, and a constant 7.7 dB apart from the render;
 *  - inside kPresenceBandDb of the median nothing moves (the variety of the voices round stays); outside, the
 *    lines are brought to the band's edge, at most kPresenceCutDb down or kPresenceLiftDb up.
 *
 * **The cap, measured rather than widened (20.09.2026, round "climax-polish").** The brightest of 30 tracks
 * (5 styles, 2 seeds, 3 tracks; the same sample as above) still sat +4.6 dB over the median after the cap --
 * is the cap too narrow, or is the brightness not in the lines to begin with? Tried both -6 and -9 dB on the
 * same 30 tracks: the three tracks the cap actually binds on moved 0.47, 1.28 and 1.30 dB at -6, and 0.55,
 * 1.53 and 1.56 dB at -9 -- 50 % more cut bought at most 0.3 dB more correction. `presenceDb` only ever reads
 * the lines against the rest (probeLoudness's kProbeLines/kProbeRest split); a track whose brightness sits in
 * its acid, its percussion or its pad instead has little for a deeper cut on the lines to remove, however
 * deep it goes -- diminishing returns, not a narrow cap. Left at -6/+3: widening it would have cost more
 * headroom on tracks it does help (the two with all four lines present moved the most) for a track like
 * this one that would stay bright regardless. A cut across the *rest* would reach it, but that is a
 * different, larger change -- it touches the acid and the kit's own recipes, not a level on top of them --
 * and the voices round's own diversity is deliberately outside matchPresence's reach (see above).
 */
void Composer::matchPresence(const ParamStore& p, TrackPlan& t, const double* linesReading, const double* restReading) const
{
    t.presenceDb = 0.0;
    t.presenceAfterDb = 0.0;
    t.presenceGainDb = 0.0f;
    // Measured whenever the level match runs, so that a render with the match off still reports where each
    // track stands (phos_render --tracks); corrected only with compose.presence_match on.
    // The two readings may have been rendered already, side by side (measureTrack).
    double lines[2] = {}, rest[2] = {};
    if (linesReading != nullptr && restReading != nullptr) {
        for (int i = 0; i < 2; ++i) { lines[i] = linesReading[i]; rest[i] = restReading[i]; }
    } else {
        probeLoudness(p, t, kProbeLines, 0.0f, lines);
        probeLoudness(p, t, kProbeRest, 0.0f, rest);
    }
    const double pl = lines[0], ll = lines[1], pr = rest[0], lr = rest[1];
    if (pl + pr <= 0.0 || ll + lr <= 0.0) return;
    t.presenceDb = t.presenceAfterDb = 10.0 * std::log10((pl + pr) / (ll + lr)) - kPresenceRefDb;
    if (!p.getBool(p.base(Module::Compose) + compose::PresenceMatch)) return;
    if (std::fabs(t.presenceDb) <= kPresenceBandDb || pl <= 0.0) return;
    // The band's edge on the side the track is on, as a ratio of the two bands' powers.
    const double want = std::pow(10.0, (kPresenceRefDb + (t.presenceDb > 0.0 ? kPresenceBandDb : -kPresenceBandDb)) / 10.0);
    const double num = want * lr - pr, den = pl - want * ll;
    double g = (num > 0.0 && den > 0.0) ? num / den : (t.presenceDb > 0.0 ? 0.0 : 1e9);
    const double db = g > 0.0 ? 10.0 * std::log10(g) : -kPresenceCutDb;
    t.presenceGainDb = static_cast<float>(std::clamp(db, -kPresenceCutDb, kPresenceLiftDb));
    // Where the correction lands, by the same linear model: the band's edge unless a cap stopped it. The
    // plans carry it so that a check (testPresence) can see the match's own prediction, not only its gain.
    const double gain = std::pow(10.0, static_cast<double>(t.presenceGainDb) / 10.0);
    t.presenceAfterDb = 10.0 * std::log10((gain * pl + pr) / (gain * ll + lr)) - kPresenceRefDb;
}

/**
 * The audibility match (23.09.2026, round "Hoerbarkeit"; compose.audibility_match). The level match brings each
 * part to the loudness it had in the first track, the presence match the lines to the reference's brightness --
 * neither asks what a listener hears of a line *inside* the mix. The audibility meter (Audibility.h) does: on seed
 * 1 it read the counter in drop 2 as heard at 0.68 of its own loudness (not masked) but at 21 units against the
 * lead's 60, and the arp at 8 -- the user's "counter barely audible" was a level, not a masking problem.
 *
 * So one more probe of the drops (the presence probe's bars, every part, the master's dynamics off, the stems
 * tapped) measures each line's partial loudness in the mix, and a line that stays under its share of the lead's
 * is lifted: the counter to 0.6 of the lead (it answers the lead and should read as its partner), the arp and the
 * stab to 0.35 (they colour, they do not lead). The step assumes loudness grows with the 0.3rd power of intensity
 * (Stevens' law for loudness), is capped at kAudibleLiftDb and only ever lifts; a track without a lead in its
 * drops has nothing to measure against and is left alone. Before Auto Gain, whose probes then hear the lift.
 */
void Composer::matchAudibility(const ParamStore& p, TrackPlan& t) const
{
    for (float& v : t.audibilityLiftDb) v = 0.0f;
    for (double& v : t.audibleInMix) v = 0.0;
    if (!p.getBool(p.base(Module::Compose) + compose::AudibilityMatch)) return;
    double inMix[kMelodyParts] = {};
    probeLoudness(p, t, kProbeAudible, 0.0f, nullptr, inMix);
    for (int k = 0; k < kMelodyParts; ++k) t.audibleInMix[k] = inMix[k];
    const double lead = inMix[mpIndex(MelodyPart::Lead)];
    if (!(lead > 0.5)) return;
    for (const AudibleWant& w : kAudibleWant) {
        const int k = mpIndex(w.part);
        if (!t.melody.present[k] || inMix[k] <= 0.05) continue;   // not in the probe's drop bars
        const double f = w.share * lead / inMix[k];
        if (f <= 1.0) continue;
        const float lift = static_cast<float>(std::min(kAudibleLiftDb, 10.0 / 0.3 * std::log10(f)));
        t.partGainDb[k] += lift;
        t.audibilityLiftDb[k] = lift;
    }
}

/**
 * @brief Everything a plan learns from probe renders: the level match, the presence match and Auto Gain.
 *
 * Until 20.09.2026 these were twelve renders one after the other, 12 s per track. What each probe *reads*
 * decides what may run at the same time (round "speed"; the maths of every step is where it was):
 *
 *  1. the foundation and the melodic parts play the track with every correction taken out (probeLoudness
 *     zeroes gainDb, partGainDb and presenceGainDb for them), so they depend on nothing measured and on
 *     each other not at all -- one stage, up to eight at once;
 *  2. the two presence probes keep the level corrections (the balance they measure is the one those make),
 *     so they wait for stage 1 and then run side by side;
 *  3. the first mix probe plays the track with *all* corrections, the presence gain on the lines included
 *     (trackStartControls), so it waits for stage 2 -- unless compose.presence_match is off, when that gain
 *     is zero by construction and the mix probe runs beside the presence probes;
 *  4. the second mix probe needs the first one's reading for its gain, so it is last and alone.
 *
 * The longest chain is then 2 + 8 + 16 + 16 bars instead of 64 to 72: measured 10.9 s against 15.3 s for the
 * listening seed's first track beside another round's load (the two mix probes are 8.2 s of it and cannot
 * overlap). Starting the first mix probe early on the guess that the presence gain will be zero was
 * measured and dropped: the guess holds for 17 of 44 plans (seeds 1..6 and the listening seed), saves 2.1 s
 * when it does, and burns a 16-bar render when it does not -- CPU a parallel test run has no spare of.
 *
 * With probe::threads() == 1 -- the library's default, the plugin and the Quest -- the old serial order
 * runs unchanged, and testProbeSchedule compares the two bit for bit. The tasks never touch the plan: each
 * reads a snapshot taken before its stage and writes a slot of its own, because a probe copies the whole
 * plan and another task writing one of its fields meanwhile would be a data race even where the value is
 * not used.
 */
void Composer::measureTrack(const ParamStore& p, TrackPlan& t) const
{
    // Live, every track is played before it is measured (completeMeasurement, Composer.h). Until 26.09.2026 only the
    // first one was: a later track measured itself here, on the composer thread, after measuring the first one if
    // that was still deferred -- 15 to 30 s during which nothing filled the engine's rings, eight bars (13 s) deep.
    // The user heard it as the playback stalling ("die Engine scheint manchmal nicht mit dem Generieren der Noten
    // hinterherzukommen"): after a jump close to a track's end the transport stood for 29.6 s (hosttest --part live).
    if (deferMaster_) {
        t.measureDeferred = true;
        t.masterDeferred = true;
        t.masterGainDb = 0.0f;
        return;
    }
    double mix0 = 0.0;
    const bool mixEarly = measureLevels(p, t, &mix0);
    // Auto Gain's two readings, the second after the first. `mixEarly` means the first ran beside the presence probes.
    matchMaster(p, t, mixEarly ? &mix0 : nullptr);
}

bool Composer::completeMeasurement(const ParamStore& p, int index) const
{
    if (index < 0 || index >= static_cast<int>(plans_.size())) return false;
    TrackPlan& t = plans_[static_cast<size_t>(index)];
    if (!t.measureDeferred) return false;
    // Every later track is matched against the first one's measurements (plans_ is a deque: t stays valid).
    if (index > 0) completeMeasurement(p, 0);
    t.measureDeferred = false;
    measureLevels(p, t, nullptr);
    t.correctionsPending = true;
    return true;
}

bool Composer::adoptMeasurement(int index, const TrackPlan& m, uint64_t generation) const
{
    if (generation != planGeneration_ || index < 0 || index >= static_cast<int>(plans_.size())) return false;
    TrackPlan& t = plans_[static_cast<size_t>(index)];
    t.loudness = m.loudness;
    t.gainDb = m.gainDb;
    for (int k = 0; k < kMelodyParts; ++k) {
        t.partLoudness[k] = m.partLoudness[k];
        t.partGainDb[k] = m.partGainDb[k];
        t.audibleInMix[k] = m.audibleInMix[k];
        t.audibilityLiftDb[k] = m.audibilityLiftDb[k];
    }
    t.presenceDb = m.presenceDb;
    t.presenceAfterDb = m.presenceAfterDb;
    t.presenceGainDb = m.presenceGainDb;
    t.mixLoudness = m.mixLoudness;
    t.masterGainDb = m.masterGainDb;
    t.measureDeferred = m.measureDeferred;
    t.masterDeferred = m.masterDeferred;
    t.correctionsPending = true;
    return true;
}

bool Composer::measureLevels(const ParamStore& p, TrackPlan& t, double* mix0Out) const
{
    const int cb = p.base(Module::Compose);
    const bool level = p.getBool(cb + compose::LevelMatch);
    const bool first = t.index == 0;
    // The level match's arithmetic, once for both orders. The first track is the reference: it measures
    // every part and corrects nothing.
    auto trackGain = [&] {
        if (!first) t.gainDb = static_cast<float>(std::clamp(plans_[0].loudness - t.loudness, -9.0, 9.0));
    };
    // A part the track does not use needs no correction (the first track measures all of them: it is the
    // reference for every later one).
    auto partProbed = [&](int k) { return first || t.melody.present[k]; };
    // Each melodic part against the same part in the first track; the track gain applies to it as well,
    // so it is taken back out.
    auto partGain = [&](int k) {
        if (first) return;
        if (!t.melody.present[k]) { t.partLoudness[k] = -120.0; t.partGainDb[k] = 0.0f; return; }
        const bool measurable = plans_[0].partLoudness[k] > -60.0 && t.partLoudness[k] > -60.0;
        t.partGainDb[k] = measurable ? static_cast<float>(std::clamp(plans_[0].partLoudness[k] - t.partLoudness[k] - t.gainDb, -12.0, 12.0)) : 0.0f;
    };

    if (probe::threads() <= 1) {
        // The serial order of before 20.09.2026.
        if (level) {
            t.loudness = probeLoudness(p, t);
            trackGain();
            for (int k = 0; k < kMelodyParts; ++k) {
                if (partProbed(k)) t.partLoudness[k] = probeLoudness(p, t, k);
                partGain(k);
            }
            matchPresence(p, t);
            matchAudibility(p, t);
        }
        return false;
    }

    // The shared loads are not thread-safe (Probe.h): on this thread, before any worker exists.
    probe::warmSharedData();
    const bool autoGain = p.getBool(p.base(Module::Master) + master::AutoGain);
    std::vector<std::function<void()>> tasks;
    if (level) {
        // Stage 1: the foundation and the parts.
        const TrackPlan snap = t;
        double loudness = 0.0, partLoudness[kMelodyParts] = {};
        tasks.push_back([&] { loudness = probeLoudness(p, snap); });
        for (int k = 0; k < kMelodyParts; ++k)
            if (partProbed(k)) tasks.push_back([&, k] { partLoudness[k] = probeLoudness(p, snap, k); });
        probe::runAll(tasks);
        t.loudness = loudness;
        trackGain();
        for (int k = 0; k < kMelodyParts; ++k) {
            if (partProbed(k)) t.partLoudness[k] = partLoudness[k];
            partGain(k);
        }
    }
    // Stage 2: the presence probes -- and the first mix probe beside them only where the presence match
    // cannot move the lines, because it is off (the mix probe first in the list: it is the longest).
    const bool mixEarly = mix0Out != nullptr && autoGain && level && !deferMaster_ && !p.getBool(cb + compose::PresenceMatch)
                       && !p.getBool(cb + compose::AudibilityMatch);   // both move the lines Auto Gain has to hear
    double lines[2] = {}, rest[2] = {}, mix0 = 0.0;
    if (level) {
        t.presenceGainDb = 0.0f;   // what matchPresence starts from
        const TrackPlan snap = t;
        tasks.clear();
        if (mixEarly) tasks.push_back([&] { mix0 = probeLoudness(p, snap, -2, 0.0f); });
        tasks.push_back([&] { probeLoudness(p, snap, kProbeLines, 0.0f, lines); });
        tasks.push_back([&] { probeLoudness(p, snap, kProbeRest, 0.0f, rest); });
        probe::runAll(tasks);
        matchPresence(p, t, lines, rest);
        matchAudibility(p, t);   // after the presence gain, before Auto Gain (23.09.2026)
    }
    // Stages 3 and 4 (Auto Gain) are measureTrack's. `mixEarly` puts the first of its two readings beside the
    // presence probes where the presence match cannot move the lines; a deferred track does not want it there
    // either, so it is not started.
    if (mixEarly) *mix0Out = mix0;
    return mixEarly;
}

float Composer::completeMasterGain(const ParamStore& p, int index) const
{
    if (index < 0 || index >= static_cast<int>(plans_.size())) return 0.0f;
    TrackPlan& t = plans_[static_cast<size_t>(index)];
    if (!t.masterDeferred) return t.masterGainDb;
    // The two whole-mix probes this track did not run when it was planned. matchMaster writes
    // masterGainDb and mixLoudness into the plan, which is what a later seek into this track will
    // then push at its start like any other track's.
    matchMaster(p, t);
    t.masterDeferred = false;
    return t.masterGainDb;
}

double Composer::probeLoudness(const ParamStore& p, const TrackPlan& plan, int part, float masterGainDb, double* bands, double* audible) const
{
    // The presence probe (19.09.2026, round "polish"; matchPresence): the drops as the form plays them, split
    // into the lines and the rest, so that the balance can be solved for the lines' gain without another render.
    // The audibility probe (23.09.2026, matchAudibility) plays the same bars with every part, and taps the stems.
    const bool presence = part == kProbeLines || part == kProbeRest || part == kProbeAudible;
    const bool mixLike = part == -2 || presence;
    auto isLine = [](MelodyPart mp) {
        return mp == MelodyPart::Lead || mp == MelodyPart::Counter || mp == MelodyPart::Arp || mp == MelodyPart::Stab;
    };
    constexpr double sr = 48000.0;
    // Two bars for the foundation and the parts. For the whole mix eight bars spread evenly over the track,
    // as its blocks really play -- intro and outro included, since the gated integrated loudness counts
    // them: the densest block alone over-read a track by 0.8 LU, four bars from its middle by 0.6 LU.
    // The audibility probe: 8 contiguous bars of drop 1 and 16 of drop 2 (sourceBar) -- lead and counter trade
    // phrases there, and the presence probe's one bar per group heard whichever of them its bar fell on.
    const int bars = part == -2 ? 16 : (part == kProbeAudible ? 24 : (presence ? 8 : 2));
    // The probe's inputs are collected first -- the parameter values, the controls, the notes -- and the
    // engine is built only when they have to be rendered (20.09.2026, round "speed"): with the probe cache
    // on (Probe.h) the same inputs may already have been rendered, by this process or another. The engine
    // receives exactly what it used to receive, in the same order: its two rings are independent, and
    // nothing is rendered before the last push.
    ParamStore ps;
    ps.copyValuesFrom(p);
    const int mb = p.base(Module::Mix);
    ps.set(mb + mix::KickMute, 0.0f);
    ps.set(mb + mix::BassMute, 0.0f);
    ps.set(mb + mix::TrackGain, 0.0f);
    ps.set(mb + mix::PercMute, 0.0f);
    // The foundation (part -1): kick, bass and percussion. A melodic part: that part alone. The whole mix
    // (part -2): everything with its corrections, through the master. The first two are measured before
    // the master's dynamics, which would bend the relation between gain and loudness.
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart mp = static_cast<MelodyPart>(k);
        const int mute = mp == MelodyPart::Acid ? static_cast<int>(mix::AcidMute) : mix::polyMute(melodyPoly(mp));
        const bool on = k == part || part == -2 || part == kProbeAudible || (part == kProbeLines && isLine(mp)) || (part == kProbeRest && !isLine(mp));
        ps.set(mb + mute, on ? 0.0f : 1.0f);
    }
    ps.set(mb + mix::SfxMute, 1.0f);
    const int ms = p.base(Module::Master);
    if (part != -2) {
        ps.set(ms + master::CompRatio, 1.0f);
        ps.set(ms + master::Limiter, 0.0f);
        ps.set(ms + master::Clip, 0.0f);
        ps.set(ms + master::Clipper, 0.0f);
    }
    if (part >= 0 || part == kProbeLines) {
        ps.set(mb + mix::KickMute, 1.0f);
        ps.set(mb + mix::BassMute, 1.0f);
        ps.set(mb + mix::PercMute, 1.0f);
    }
    TempoMap tm;
    tm.setConstant(plan.bpm);

    std::vector<ControlEvent> controls;
    TrackPlan neutral = plan;
    neutral.masterGainDb = masterGainDb;
    // The presence probe keeps the parts' level corrections: the balance it measures is the one they make.
    if (!mixLike) {
        neutral.gainDb = 0.0f;
        neutral.presenceGainDb = 0.0f;
        for (float& g : neutral.partGainDb) g = 0.0f;
    }
    trackStartControls(p, neutral, 0.0, controls);
    arcControls(p, neutral, 0, 0.0, false, controls);

    const int root = bassRootNote(plan.key, p.getInt(p.base(Module::Compose) + compose::BassRegister));
    const BassPatternDef& pat = kBassPatterns[plan.primaryPattern];
    std::vector<NoteEvent> notes;
    for (int beat = 0; beat < bars * kBeatsPerBar; ++beat) {
        NoteEvent k;
        k.beat = beat;
        k.part = Part::Kick;
        k.velocity = 127;
        notes.push_back(k);
        for (int s = 0; s < pat.count; ++s) {
            const double next = s + 1 < pat.count ? pat.pos[s + 1] : 1.0;
            NoteEvent n;
            n.beat = beat + pat.pos[s];
            n.length = static_cast<float>((next - pat.pos[s]) * plan.gate);
            n.part = Part::Bass;
            // The probe plays the track's own bass line, because a learned phrase is not the root
            // over and over: a bar of octaves is measurably quieter through the same ladder than a
            // bar of roots, and the level match between tracks would inherit that error. With the
            // pattern generator the phrase is absent and this is the root, as it always was.
            const int rel = plan.bassNeural
                                ? plan.bassRel[0][((beat / kBeatsPerBar % kBassPhraseBars) * kBeatsPerBar
                                                   + beat % kBeatsPerBar) * 3 + s]
                                : 0;   // beat runs over the whole probe, so beat/4 is its bar
            n.pitch = static_cast<uint8_t>(std::clamp(root + rel, 0, 127));
            n.velocity = 110;
            notes.push_back(n);
        }
    }
    // Which bar of the track each probe bar is: 49 and 50 (the percussion's layers are in) for the foundation
    // and the parts, the middle bar of the block at each fifth of the track for the whole mix.
    // Four contiguous windows of four bars, spread evenly over the track. Contiguous, because a pad
    // holds its chord over several bars and a reverb tail runs on: single bars cut from the middle of a
    // breakdown read far quieter than the same bar does in the track.
    auto sourceBar = [&](int b) {
        if (part == kProbeAudible) {
            int d1 = -1, d2 = -1;
            for (int i = 0; i < plan.form.count; ++i)
                if (plan.form.section[i].type == SectionType::Drop) { if (d1 < 0) d1 = i; d2 = i; }
            const Section& s = plan.form.section[std::max(0, b < 8 ? d1 : d2)];
            const int in = 8 + (b < 8 ? b : b - 8);   // from the second group on: the drop's first bars carry the impact
            return s.startBar + std::min(s.bars - 1, in);
        }
        if (presence) {
            // Four bars of drop 1 and four of drop 2 (the last drop, where a form has only one): the fifth bar
            // of each of their first four eight-bar groups. One per group, because the stab and the counter
            // take whole groups by turns (Form.cpp), and two bars from the middle heard one of them or neither
            // -- on the calibration tracks the probe then read the brightest tracks 2 dB low.
            int d1 = -1, d2 = -1;
            for (int i = 0; i < plan.form.count; ++i)
                if (plan.form.section[i].type == SectionType::Drop) { if (d1 < 0) d1 = i; d2 = i; }
            const Section& s = plan.form.section[std::max(0, b < 4 ? d1 : d2)];
            return s.startBar + std::min(s.bars - 1, 8 * (b % 4) + 4);
        }
        if (part != -2) return 49 + b;
        // The first window starts on bar 0 and the last ends on the last bar, so that the intro and
        // the outro count: they are a tenth of the track and quieter than everything else, and a probe
        // that skipped them aimed about 1 LU too high.
        const int window = ((b / 4) * (plan.bars - 4) / 3) / 4 * 4;
        return window + b % 4;
    };
    // The bar plan of a probe bar: the real instrumentation matrix for the whole mix, everything the
    // track has for a single part or the foundation (those measure a sound, not an arrangement).
    auto probeBar = [&](int source) {
        if (!mixLike) {
            BarPlan bp = allPartsBar(plan.melody);
            bp.percLayers = plan.perc.layers;
            // A part's own probe plays that part whether or not the track uses it (19.09.2026): every
            // track has the material of every part, and the first track is the reference the later
            // ones are matched against -- a first track without a stab left every later stab unmatched.
            if (part >= 0) bp.parts = bp.partsNext = partBit(static_cast<MelodyPart>(part));
            return bp;
        }
        return planBar(plan.form, availabilityOf(plan), plan.sectionSeed, source);
    };
    // The foundation probe plays as many percussion layers as the track's first core does (19.09.2026).
    // It used to play all the track has; a core often plays fewer, and the probe then over-read the
    // track by up to 0.9 LU -- by different amounts from track to track, so the level match matched
    // the percussion plan instead of the sound. Measured in testVariety's level-match check: 0.81 LU of
    // spread with all layers, 0.40 with the core's (docs/rounds/2026-09.md, 19.09.2026). The round of 18.09.2026
    // had found the same limit from the other side: louder toms and congas widened the spread because
    // "the probe does not see how much of a track toms and congas play".
    //
    // 19.09.2026 (round "voices"): the first *drop*, where the track has one, and the first core only
    // where it has none. A track whose first core is a groove (the Full-On body) measured the groove's
    // thinner kit -- two layers plus one per eight-bar group -- and was lifted for it: on the listening
    // seed track 2 by +2.0 dB where its drop, kick, bass and kit alone, is only 0.3 LU quieter than
    // track 1's (rendered, master dynamics off: -11.8 against -11.5 LUFS unmatched, -9.8 matched). The
    // gain acts on kick, bass and percussion only, so the drop stood 1.7 dB hot and the level match of
    // its melodic parts, which takes the track gain back out, could not see it.
    int coreLayers = plan.perc.layers;
    int coreAt = -1;
    for (int pass = 0; pass < 2 && coreAt < 0; ++pass)
        for (int i = 0; i < plan.form.count && coreAt < 0; ++i) {
            const SectionType st = plan.form.section[i].type;
            if (st == SectionType::Drop || (pass == 1 && st == SectionType::Groove)) coreAt = i;
        }
    if (coreAt >= 0) {
        const Section& core = plan.form.section[coreAt];
        coreLayers = planBar(plan.form, availabilityOf(plan), plan.sectionSeed, core.startBar + std::min(4, core.bars - 1)).percLayers;
    }
    for (int b = 0; b < bars; ++b) {
        BarPlan bp = probeBar(sourceBar(b));
        if (part == -1) bp.percLayers = std::min(bp.percLayers, coreLayers);
        // The whole-mix probe has to hear the loudness side of the energy arc as well, or Auto Gain
        // would aim at a track that is louder than the one the form really plays (measured: 1.3 LU).
        if (part == -2) {
            const Section& sec = plan.form.section[std::clamp(bp.index, 0, kMaxSections - 1)];
            const ParamDesc& g = p.desc(mb + mix::TrackGain);
            ControlEvent c;
            c.beat = static_cast<double>(b) * kBeatsPerBar;
            c.param = static_cast<int16_t>(mb + mix::TrackGain);
            c.kind = ControlEvent::Kind::Offset;
            // With the climax trim at the probe bar's place in its section (Form.h, kDrop1HoldDb).
            const double u = sec.bars > 1 ? static_cast<double>(sourceBar(b) - sec.startBar) / sec.bars : 0.0;
            c.value = (plan.gainDb + energyGainDb(0.5f * (sec.energy + sec.energyTo)) + sectionTrimDb(plan.form, bp.index, u))
                    / (g.maxValue - g.minValue);
            controls.push_back(c);
        }
        PercBarSpec spec;
        spec.layers = bp.percLayers;
        spec.fills = false;
        spec.hatsDense = bp.hatsDense;
        spec.rollBar = bp.rollBar;
        spec.rollBars = bp.rollBars;
        spec.pdb = bp.pdb;
        spec.cutBeats = bp.cutBeats;
        spec.quietHats = bp.quietHats;
        spec.shaker = bp.shaker;
        spec.offbeatHat = bp.offbeatHat;
        spec.hatLevel = bp.hatLevel;
        spec.openHats = bp.openHats;
        spec.ride = bp.ride;
        composePercBar(p, plan.perc, plan.percSeed, b, sourceBar(b), plan.bpm, plan.key, plan.scale, spec, notes);
    }
    // The melodic parts: everything the track has for their own measurement, as the form plays them
    // for the whole mix.
    if (part >= 0 || mixLike) {
        std::vector<NoteEvent> mel;
        for (int b = 0; b < bars; ++b) {
            const BarPlan bp = probeBar(sourceBar(b));
            // The drone plays one held chord per run (MelodyContext::droneBars), which a probe bar
            // taken out of the middle of a run would never start: the part probe holds it over its two
            // bars in the upper octave, the mix probe over each four-bar window in the octave the form has
            // there (19.09.2026).
            MelodyContext ctx;
            if (mixLike) {
                ctx = melodyContext(plan, bp, sourceBar(b));
                if (b % 4 == 0 && ctx.droneBars == 0 && (bp.parts & partBit(MelodyPart::Drone)) != 0) {
                    ctx.droneBars = 4;
                    ctx.droneLow = droneLowAt(plan, sourceBar(b));
                }
            } else if (b == 0) {
                ctx.droneBars = bars;
            }
            composeMelodyBar(p, plan.melody, b, mixLike ? sourceBar(b) : 48 + b, plan.scale, bp, mel, ctx);
        }
        for (const NoteEvent& n : mel) if (mixLike || n.part == melodyScorePart(static_cast<MelodyPart>(part))) notes.push_back(n);
    }
    // The presence probe hears each drop's own section controls -- the energy's filter arc, the section's drawn
    // wobble, drop 2's lift (Form.h, kClimaxOpen) -- at once rather than as the ramps a whole section plays them
    // over; without them the probe read the brighter tracks up to 2 dB low against the render (calibration, above
    // matchPresence). Only events inside the probe, in time order behind the track-start controls at beat 0.
    if (presence) {
        std::vector<ControlEvent> sc;
        for (int b = 0; b < bars; b += 4) sectionControls(p, neutral, probeBar(sourceBar(b)), static_cast<double>(b) * kBeatsPerBar, sc);
        std::vector<ControlEvent> keep;
        for (ControlEvent c : sc) {
            if (c.beat >= static_cast<double>(bars) * kBeatsPerBar) continue;
            c.length = 0.0f;
            keep.push_back(c);
        }
        std::stable_sort(keep.begin(), keep.end(), [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; });
        for (const ControlEvent& c : keep) controls.push_back(c);
    }
    // One ring, so one order: the engine plays from the head of the ring.
    std::stable_sort(notes.begin(), notes.end(), noteLess);
    const int total = static_cast<int>(tm.secondsAt(bars * kBeatsPerBar) * sr);

    // The cache key: everything the render below depends on (Probe.h). Field by field, so that padding
    // bytes never enter it; with the counts, so that a note cannot pass for a control.
    // The audibility probe returns a reading per part, more than the cache's three values: never cached.
    const bool cached = probe::cacheEnabled() && audible == nullptr;
    probe::Key key;
    if (cached) {
        // The shared data is hashed as it is loaded *now*, so it has to be loaded before the key is made and
        // not by the engine after it: "not loaded yet" and "the file is missing" would otherwise be one key
        // for two different renders. A flag test when it has happened (and it has, before any worker runs).
        probe::warmSharedData();
        probe::Hasher h;
        h.text("phos probe 1");   // the layout of this key
        h.text(probe::buildId());
        h.add(probe::sharedDataId());
        h.add(sr);
        h.add(static_cast<int32_t>(512));
        h.add(static_cast<int32_t>(total));
        h.add(static_cast<uint8_t>(bands != nullptr ? 1 : 0));
        h.add(plan.bpm);
        h.add(static_cast<int32_t>(ps.count()));
        for (int i = 0; i < ps.count(); ++i) h.add(ps.get(i));
        h.add(static_cast<uint64_t>(controls.size()));
        for (const ControlEvent& c : controls) { h.add(c.beat); h.add(c.length); h.add(c.value); h.add(c.param); h.add(c.kind); }
        h.add(static_cast<uint64_t>(notes.size()));
        for (const NoteEvent& n : notes) { h.add(n.beat); h.add(n.length); h.add(n.part); h.add(n.lane); h.add(n.pitch); h.add(n.velocity); h.add(n.flags); }
        key = h.finish();
        double v[3];
        if (probe::cacheLookup(key, v)) {
            if (bands != nullptr) { bands[0] = v[1]; bands[1] = v[2]; }
            return v[0];
        }
    }

    auto engine = std::make_unique<Engine>();
    engine->prepare(sr, 512);
    engine->params().copyValuesFrom(ps);
    engine->setTempoMap(tm);
    for (const ControlEvent& c : controls) engine->pushControl(c);
    for (const NoteEvent& n : notes) engine->pushEvent(n);
    LoudnessMeter meter;
    meter.prepare(sr);
    std::vector<float> L(512), R(512);
    // Band powers for the presence match: 1.5..6 kHz and 40..140 Hz, each a fourth-order band (two
    // second-order high passes and two low passes, Butterworth Q), the bands of Tools/metrics.py.
    Svf bandFilter[2][2][4];
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k) {
            bandFilter[0][c][k].setQ(1500.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[0][c][2 + k].setQ(6000.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[1][c][k].setQ(40.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[1][c][2 + k].setQ(140.0f, 0.7071f, static_cast<float>(sr));
        }
    double bandSum[2] = {};
    // The stems for the audibility meter (Engine.h, StemTap; Audibility.h), only when they are asked for.
    std::vector<std::vector<float>> stemL, stemR;
    StemTap tap;
    std::unique_ptr<AudibilityMeter> aud;
    if (audible != nullptr) {
        stemL.assign(kNumStems, std::vector<float>(512));
        stemR.assign(kNumStems, std::vector<float>(512));
        for (int s = 0; s < kNumStems; ++s) { tap.L[s] = stemL[static_cast<size_t>(s)].data(); tap.R[s] = stemR[static_cast<size_t>(s)].data(); }
        engine->setStemTap(&tap);
        aud = std::make_unique<AudibilityMeter>(kNumStems, sr);
    }
    for (int done = 0; done < total; done += 512) {
        // A background copy that is being torn down (setAbortFlag): stop, and cache nothing.
        if (abort_ != nullptr && abort_->load(std::memory_order_relaxed)) return 0.0;
        const int n = std::min(512, total - done);
        engine->process(L.data(), R.data(), n);
        meter.process(L.data(), R.data(), n);
        if (aud != nullptr) {
            const float* sl[kNumStems];
            const float* sr2[kNumStems];
            for (int s = 0; s < kNumStems; ++s) { sl[s] = stemL[static_cast<size_t>(s)].data(); sr2[s] = stemR[static_cast<size_t>(s)].data(); }
            aud->add(sl, sr2, n);
        }
        if (bands == nullptr) continue;
        for (int c = 0; c < 2; ++c) {
            const float* x = c == 0 ? L.data() : R.data();
            for (int i = 0; i < n; ++i)
                for (int band = 0; band < 2; ++band) {
                    float y = x[i], lp, bp, hp;
                    for (int k = 0; k < 2; ++k) { bandFilter[band][c][k].tick(y, lp, bp, hp); y = hp; }
                    for (int k = 2; k < 4; ++k) y = bandFilter[band][c][k].lp(y);
                    bandSum[band] += static_cast<double>(y) * y;
                }
        }
    }
    if (bands != nullptr) { bands[0] = bandSum[0]; bands[1] = bandSum[1]; }
    if (aud != nullptr) {
        engine->setStemTap(nullptr);
        for (int k = 0; k < kMelodyParts; ++k) audible[k] = aud->read(static_cast<int>(melodyScorePart(static_cast<MelodyPart>(k)))).inMix;
    }
    const double integrated = meter.read().integrated;
    if (cached) {
        const double v[3] = { integrated, bandSum[0], bandSum[1] };
        probe::cacheStore(key, v);
    }
    return integrated;
}

} // namespace phos
