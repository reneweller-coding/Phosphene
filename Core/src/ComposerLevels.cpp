/**
 * @file ComposerLevels.cpp
 * @brief Measuring a track: the level match, presence, audibility and Auto Gain, and the probe renders they read.
 * @author Rene Weller
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

namespace phos
{

namespace
{

constexpr double PresenceRefDb = -7.43;   ///< the probe's reading at the reference median (calibrated, see above)
constexpr double PresenceBandDb = 1.5;    ///< no correction within this distance of the median
constexpr double PresenceCutDb = 6.0;     ///< the most the lines are taken down (kept at 6, not widened -- see above)
constexpr double PresenceLiftDb = 3.0;    ///< the most they are brought up

constexpr double AudibleLiftDb = 4.0;   ///< the most one line is lifted
/** @brief A line that should be heard and the share of the time it wants to be. */
struct AudibleWant
{
    MelodyPartE m_part;    ///< the line
    double      m_share;   ///< the share it wants
};
/** @brief The lines that should be heard, with the share each wants. */
constexpr AudibleWant AudibleWants[] = { { MelodyPartE::COUNTER, 0.60 },
                                         { MelodyPartE::ARP, 0.35 },
                                         { MelodyPartE::STAB, 0.35 } };

}   // namespace

void Composer::matchMaster( const ParamStore &p, TrackPlan &t, const double *firstReading ) const
{
    const int ms = p.base( ModuleE::MASTER );
    t.m_masterGainDb = 0.0f;
    if ( !p.getBool( ms + master::AUTO_GAIN ) ) return;
    const double target = p.get( ms + master::TARGET_LUFS );
    // Measure, correct, measure again, and take a secant step between the two readings.
    // The first reading may have been rendered already, beside the presence probes (measureTrack).
    const double l0 = firstReading != nullptr ? *firstReading : probeLoudness( p, t, -2, 0.0f );
    t.m_mixLoudness = l0;
    if ( l0 < -60.0 ) return;
    const double g1 = std::clamp( target - l0, -18.0, 18.0 );
    const double l1 = probeLoudness( p, t, -2, static_cast<float>( g1 ) );
    const double slope = std::fabs( g1 ) > 0.05 ? ( l1 - l0 ) / g1 : 1.0;
    const double g2 = g1 + ( target - l1 ) / std::clamp( slope, 0.2, 1.0 );
    t.m_masterGainDb = static_cast<float>( std::clamp( g2, -18.0, 18.0 ) );
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
 *  - the estimate is read against the reference median through PresenceRefDb: the probe's raw ratio minus it is
 *    the rendered drops' presence (the power mean of the two drops, Tools/metrics.py bands) over 30 calibration
 *    tracks (5 styles, 2 seeds, 3 tracks each; the match off) with a median error of -0.04 dB, interquartile
 *    -0.33 .. +0.33 dB, standard deviation 0.81, correlation 0.97; the three worst read the render 1.8 .. 2.5 dB
 *    low (bright tracks whose drop 2 carries more than the probe's four bars show). Two bars from the middle of each drop without the section controls
 *    had read the bright tracks up to 2.4 dB low, and a constant 7.7 dB apart from the render;
 *  - inside PresenceBandDb of the median nothing moves (the variety of the voices round stays); outside, the
 *    lines are brought to the band's edge, at most PresenceCutDb down or PresenceLiftDb up.
 *
 * **The cap, measured rather than widened (20.09.2026, round "climax-polish").** The brightest of 30 tracks
 * (5 styles, 2 seeds, 3 tracks; the same sample as above) still sat +4.6 dB over the median after the cap --
 * is the cap too narrow, or is the brightness not in the lines to begin with? Tried both -6 and -9 dB on the
 * same 30 tracks: the three tracks the cap actually binds on moved 0.47, 1.28 and 1.30 dB at -6, and 0.55,
 * 1.53 and 1.56 dB at -9 -- 50 % more cut bought at most 0.3 dB more correction. `presenceDb` only ever reads
 * the lines against the rest (probeLoudness's ProbeLines/ProbeRest split); a track whose brightness sits in
 * its acid, its percussion or its pad instead has little for a deeper cut on the lines to remove, however
 * deep it goes -- diminishing returns, not a narrow cap. Left at -6/+3: widening it would have cost more
 * headroom on tracks it does help (the two with all four lines present moved the most) for a track like
 * this one that would stay bright regardless. A cut across the *rest* would reach it, but that is a
 * different, larger change -- it touches the acid and the kit's own recipes, not a level on top of them --
 * and the voices round's own diversity is deliberately outside matchPresence's reach (see above).
 */
void Composer::matchPresence( const ParamStore &params, TrackPlan &plan, const double *linesReading,
                              const double *restReading ) const
{
    plan.m_presenceDb = 0.0;
    plan.m_presenceAfterDb = 0.0;
    plan.m_presenceGainDb = 0.0f;
    // Measured whenever the level match runs, so that a render with the match off still reports where each
    // track stands (phos_render --tracks); corrected only with compose.presence_match on.
    // The two readings may have been rendered already, side by side (measureTrack).
    double lines[2] = {}, rest[2] = {};
    if ( linesReading != nullptr && restReading != nullptr )
    {
        for ( int i = 0; i < 2; ++i )
        {
            lines[i] = linesReading[i];
            rest[i] = restReading[i];
        }
    }
    else
    {
        probeLoudness( params, plan, ProbeLines, 0.0f, lines );
        probeLoudness( params, plan, ProbeRest, 0.0f, rest );
    }
    const double pl = lines[0], ll = lines[1], pr = rest[0], lr = rest[1];
    if ( pl + pr <= 0.0 || ll + lr <= 0.0 ) return;
    plan.m_presenceDb = plan.m_presenceAfterDb = 10.0 * std::log10( ( pl + pr ) / ( ll + lr ) ) - PresenceRefDb;
    if ( !params.getBool( params.base( ModuleE::COMPOSE ) + compose::PRESENCE_MATCH ) ) return;
    if ( std::fabs( plan.m_presenceDb ) <= PresenceBandDb || pl <= 0.0 ) return;
    // The band's edge on the side the track is on, as a ratio of the two bands' powers.
    const double want =
        std::pow( 10.0, ( PresenceRefDb + ( plan.m_presenceDb > 0.0 ? PresenceBandDb : -PresenceBandDb ) ) / 10.0 );
    const double num = want * lr - pr, den = pl - want * ll;
    double       g = ( num > 0.0 && den > 0.0 ) ? num / den : ( plan.m_presenceDb > 0.0 ? 0.0 : 1e9 );
    const double db = g > 0.0 ? 10.0 * std::log10( g ) : -PresenceCutDb;
    plan.m_presenceGainDb = static_cast<float>( std::clamp( db, -PresenceCutDb, PresenceLiftDb ) );
    // Where the correction lands, by the same linear model: the band's edge unless a cap stopped it. The
    // plans carry it so that a check (testPresence) can see the match's own prediction, not only its gain.
    const double gain = std::pow( 10.0, static_cast<double>( plan.m_presenceGainDb ) / 10.0 );
    plan.m_presenceAfterDb = 10.0 * std::log10( ( gain * pl + pr ) / ( gain * ll + lr ) ) - PresenceRefDb;
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
 * (Stevens' law for loudness), is capped at AudibleLiftDb and only ever lifts; a track without a lead in its
 * drops has nothing to measure against and is left alone. Before Auto Gain, whose probes then hear the lift.
 */
void Composer::matchAudibility( const ParamStore &params, TrackPlan &plan ) const
{
    for ( float &v : plan.m_audibilityLiftDb ) v = 0.0f;
    for ( double &v : plan.m_audibleInMix ) v = 0.0;
    if ( !params.getBool( params.base( ModuleE::COMPOSE ) + compose::AUDIBILITY_MATCH ) ) return;
    double inMix[MelodyParts] = {};
    probeLoudness( params, plan, ProbeAudible, 0.0f, nullptr, inMix );
    for ( int k = 0; k < MelodyParts; ++k ) plan.m_audibleInMix[k] = inMix[k];
    const double lead = inMix[mpIndex( MelodyPartE::LEAD )];
    if ( !( lead > 0.5 ) ) return;
    for ( const AudibleWant &w : AudibleWants )
    {
        const int k = mpIndex( w.m_part );
        if ( !plan.m_melody.m_present[k] || inMix[k] <= 0.05 ) continue;   // not in the probe's drop bars
        const double f = w.m_share * lead / inMix[k];
        if ( f <= 1.0 ) continue;
        const float lift = static_cast<float>( std::min( AudibleLiftDb, 10.0 / 0.3 * std::log10( f ) ) );
        plan.m_partGainDb[k] += lift;
        plan.m_audibilityLiftDb[k] = lift;
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
void Composer::measureTrack( const ParamStore &params, TrackPlan &plan ) const
{
    // Live, every track is played before it is measured (completeMeasurement, Composer.h). Until 26.09.2026 only the
    // first one was: a later track measured itself here, on the composer thread, after measuring the first one if
    // that was still deferred -- 15 to 30 s during which nothing filled the engine's rings, eight bars (13 s) deep.
    // The user heard it as the playback stalling ("die Engine scheint manchmal nicht mit dem Generieren der Noten
    // hinterherzukommen"): after a jump close to a track's end the transport stood for 29.6 s (hosttest --part live).
    if ( m_deferMaster )
    {
        plan.m_measureDeferred = true;
        plan.m_masterDeferred = true;
        plan.m_masterGainDb = 0.0f;
        return;
    }
    double     mix0 = 0.0;
    const bool mixEarly = measureLevels( params, plan, &mix0 );
    // Auto Gain's two readings, the second after the first. `mixEarly` means the first ran beside the presence probes.
    matchMaster( params, plan, mixEarly ? &mix0 : nullptr );
}

bool Composer::completeMeasurement( const ParamStore &p, int index ) const
{
    if ( index < 0 || index >= static_cast<int>( m_plans.size() ) ) return false;
    TrackPlan &t = m_plans[static_cast<size_t>( index )];
    if ( !t.m_measureDeferred ) return false;
    // Every later track is matched against the first one's measurements (m_plans is a deque: t stays valid).
    if ( index > 0 ) completeMeasurement( p, 0 );
    t.m_measureDeferred = false;
    measureLevels( p, t, nullptr );
    t.m_correctionsPending = true;
    return true;
}

bool Composer::adoptMeasurement( int index, const TrackPlan &m, uint64_t generation ) const
{
    if ( generation != m_planGeneration || index < 0 || index >= static_cast<int>( m_plans.size() ) ) return false;
    TrackPlan &t = m_plans[static_cast<size_t>( index )];
    t.m_loudness = m.m_loudness;
    t.m_refLoudness = m.m_refLoudness;
    t.m_gainDb = m.m_gainDb;
    for ( int k = 0; k < MelodyParts; ++k )
    {
        t.m_partLoudness[k] = m.m_partLoudness[k];
        t.m_refPartLoudness[k] = m.m_refPartLoudness[k];
        t.m_partGainDb[k] = m.m_partGainDb[k];
        t.m_audibleInMix[k] = m.m_audibleInMix[k];
        t.m_audibilityLiftDb[k] = m.m_audibilityLiftDb[k];
    }
    t.m_presenceDb = m.m_presenceDb;
    t.m_presenceAfterDb = m.m_presenceAfterDb;
    t.m_presenceGainDb = m.m_presenceGainDb;
    t.m_mixLoudness = m.m_mixLoudness;
    t.m_masterGainDb = m.m_masterGainDb;
    t.m_measureDeferred = m.m_measureDeferred;
    t.m_masterDeferred = m.m_masterDeferred;
    t.m_correctionsPending = true;
    return true;
}

bool Composer::measureLevels( const ParamStore &p, TrackPlan &t, double *mix0Out ) const
{
    const int  cb = p.base( ModuleE::COMPOSE );
    const bool level = p.getBool( cb + compose::LEVEL_MATCH );
    const bool first = t.m_index == 0;
    // The reference (26.09.2026, Composer.h refLoudness): the first track with its presets off. Measured only where it
    // plays one; otherwise it is the first track as it stands, which corrects nothing, as before.
    bool presets = false;
    for ( int k = 0; k < SoundSynths && first; ++k ) presets = presets || presetOf( p, t, k ) != nullptr;
    ParamStore recipes;
    if ( presets )
    {
        recipes.copyValuesFrom( p );
        recipes.set( cb + compose::SOUND_PRESETS, 0.0f );
    }
    const TrackPlan &ref = first ? t : m_plans[0];
    // The level match's arithmetic, once for both orders. Every track against the reference -- the first one too,
    // where it plays presets.
    auto trackGain = [&]
    {
        if ( !first || presets )
            t.m_gainDb = static_cast<float>( std::clamp( ref.m_refLoudness - t.m_loudness, -9.0, 9.0 ) );
    };
    // A part the track does not use needs no correction (the first track measures all of them: it is the
    // reference for every later one).
    auto partProbed = [&]( int k ) { return first || t.m_melody.m_present[k]; };
    // Each melodic part against the same part in the first track; the track gain applies to it as well,
    // so it is taken back out.
    auto partGain = [&]( int k )
    {
        if ( first && !presets ) return;
        if ( !first && !t.m_melody.m_present[k] )
        {
            t.m_partLoudness[k] = -120.0;
            t.m_partGainDb[k] = 0.0f;
            return;
        }
        const bool measurable = ref.m_refPartLoudness[k] > -60.0 && t.m_partLoudness[k] > -60.0;
        t.m_partGainDb[k] =
            measurable ? static_cast<float>(
                             std::clamp( ref.m_refPartLoudness[k] - t.m_partLoudness[k] - t.m_gainDb, -12.0, 12.0 ) )
                       : 0.0f;
    };

    if ( probe::threads() <= 1 )
    {
        // The serial order of before 20.09.2026.
        if ( level )
        {
            t.m_loudness = probeLoudness( p, t );
            t.m_refLoudness = presets ? probeLoudness( recipes, t ) : t.m_loudness;
            trackGain();
            for ( int k = 0; k < MelodyParts; ++k )
            {
                if ( partProbed( k ) ) t.m_partLoudness[k] = probeLoudness( p, t, k );
                t.m_refPartLoudness[k] = presets ? probeLoudness( recipes, t, k ) : t.m_partLoudness[k];
                partGain( k );
            }
            matchPresence( p, t );
            matchAudibility( p, t );
        }
        return false;
    }

    // The shared loads are not thread-safe (Probe.h): on this thread, before any worker exists.
    probe::warmSharedData();
    const bool                         autoGain = p.getBool( p.base( ModuleE::MASTER ) + master::AUTO_GAIN );
    std::vector<std::function<void()>> tasks;
    if ( level )
    {
        // Stage 1: the foundation and the parts.
        const TrackPlan snap = t;
        double          loudness = 0.0, partLoudness[MelodyParts] = {}, refLoudness = 0.0, refPart[MelodyParts] = {};
        tasks.push_back( [&] { loudness = probeLoudness( p, snap ); } );
        for ( int k = 0; k < MelodyParts; ++k )
            if ( partProbed( k ) ) tasks.push_back( [&, k] { partLoudness[k] = probeLoudness( p, snap, k ); } );
        if ( presets )
        {
            tasks.push_back( [&] { refLoudness = probeLoudness( recipes, snap ); } );
            for ( int k = 0; k < MelodyParts; ++k )
                tasks.push_back( [&, k] { refPart[k] = probeLoudness( recipes, snap, k ); } );
        }
        probe::runAll( tasks );
        t.m_loudness = loudness;
        t.m_refLoudness = presets ? refLoudness : loudness;
        trackGain();
        for ( int k = 0; k < MelodyParts; ++k )
        {
            if ( partProbed( k ) ) t.m_partLoudness[k] = partLoudness[k];
            t.m_refPartLoudness[k] = presets ? refPart[k] : t.m_partLoudness[k];
            partGain( k );
        }
    }
    // Stage 2: the presence probes -- and the first mix probe beside them only where the presence match
    // cannot move the lines, because it is off (the mix probe first in the list: it is the longest).
    const bool mixEarly = mix0Out != nullptr && autoGain && level && !m_deferMaster &&
                          !p.getBool( cb + compose::PRESENCE_MATCH ) &&
                          !p.getBool( cb + compose::AUDIBILITY_MATCH );   // both move the lines Auto Gain has to hear
    double     lines[2] = {}, rest[2] = {}, mix0 = 0.0;
    if ( level )
    {
        t.m_presenceGainDb = 0.0f;   // what matchPresence starts from
        const TrackPlan snap = t;
        tasks.clear();
        if ( mixEarly ) tasks.push_back( [&] { mix0 = probeLoudness( p, snap, -2, 0.0f ); } );
        tasks.push_back( [&] { probeLoudness( p, snap, ProbeLines, 0.0f, lines ); } );
        tasks.push_back( [&] { probeLoudness( p, snap, ProbeRest, 0.0f, rest ); } );
        probe::runAll( tasks );
        matchPresence( p, t, lines, rest );
        matchAudibility( p, t );   // after the presence gain, before Auto Gain (23.09.2026)
    }
    // Stages 3 and 4 (Auto Gain) are measureTrack's. `mixEarly` puts the first of its two readings beside the
    // presence probes where the presence match cannot move the lines; a deferred track does not want it there
    // either, so it is not started.
    if ( mixEarly ) *mix0Out = mix0;
    return mixEarly;
}

float Composer::completeMasterGain( const ParamStore &p, int index ) const
{
    if ( index < 0 || index >= static_cast<int>( m_plans.size() ) ) return 0.0f;
    TrackPlan &t = m_plans[static_cast<size_t>( index )];
    if ( !t.m_masterDeferred ) return t.m_masterGainDb;
    // The two whole-mix probes this track did not run when it was planned. matchMaster writes
    // masterGainDb and mixLoudness into the plan, which is what a later seek into this track will
    // then push at its start like any other track's.
    matchMaster( p, t );
    t.m_masterDeferred = false;
    return t.m_masterGainDb;
}

double Composer::probeLoudness( const ParamStore &p, const TrackPlan &plan, int part, float masterGainDb, double *bands,
                                double *audible ) const
{
    // The presence probe (19.09.2026, round "polish"; matchPresence): the drops as the form plays them, split
    // into the lines and the rest, so that the balance can be solved for the lines' gain without another render.
    // The audibility probe (23.09.2026, matchAudibility) plays the same bars with every part, and taps the stems.
    const bool presence = part == ProbeLines || part == ProbeRest || part == ProbeAudible;
    const bool mixLike = part == -2 || presence;
    auto       isLine = []( MelodyPartE mp )
    {
        return mp == MelodyPartE::LEAD || mp == MelodyPartE::COUNTER || mp == MelodyPartE::ARP ||
               mp == MelodyPartE::STAB;
    };
    constexpr double sr = 48000.0;
    // Two bars for the foundation and the parts. For the whole mix eight bars spread evenly over the track,
    // as its blocks really play -- intro and outro included, since the gated integrated loudness counts
    // them: the densest block alone over-read a track by 0.8 LU, four bars from its middle by 0.6 LU.
    // The audibility probe: 8 contiguous bars of drop 1 and 16 of drop 2 (sourceBar) -- lead and counter trade
    // phrases there, and the presence probe's one bar per group heard whichever of them its bar fell on.
    const int bars = part == -2 ? 16 : ( part == ProbeAudible ? 24 : ( presence ? 8 : 2 ) );
    // The probe's inputs are collected first -- the parameter values, the controls, the notes -- and the
    // engine is built only when they have to be rendered (20.09.2026, round "speed"): with the probe cache
    // on (Probe.h) the same inputs may already have been rendered, by this process or another. The engine
    // receives exactly what it used to receive, in the same order: its two rings are independent, and
    // nothing is rendered before the last push.
    ParamStore ps;
    ps.copyValuesFrom( p );
    const int mb = p.base( ModuleE::MIX );
    ps.set( mb + mix::KICK_MUTE, 0.0f );
    ps.set( mb + mix::BASS_MUTE, 0.0f );
    ps.set( mb + mix::TRACK_GAIN, 0.0f );
    ps.set( mb + mix::PERC_MUTE, 0.0f );
    // The foundation (part -1): kick, bass and percussion. A melodic part: that part alone. The whole mix
    // (part -2): everything with its corrections, through the master. The first two are measured before
    // the master's dynamics, which would bend the relation between gain and loudness.
    for ( int k = 0; k < MelodyParts; ++k )
    {
        const MelodyPartE mp = static_cast<MelodyPartE>( k );
        const int         mute =
            mp == MelodyPartE::ACID ? static_cast<int>( mix::ACID_MUTE ) : mix::polyMute( melodyPoly( mp ) );
        const bool on = k == part || part == -2 || part == ProbeAudible || ( part == ProbeLines && isLine( mp ) ) ||
                        ( part == ProbeRest && !isLine( mp ) );
        ps.set( mb + mute, on ? 0.0f : 1.0f );
    }
    ps.set( mb + mix::SFX_MUTE, 1.0f );
    const int ms = p.base( ModuleE::MASTER );
    if ( part != -2 )
    {
        ps.set( ms + master::COMP_RATIO, 1.0f );
        ps.set( ms + master::LIMITER, 0.0f );
        ps.set( ms + master::CLIP, 0.0f );
        ps.set( ms + master::CLIPPER, 0.0f );
    }
    if ( part >= 0 || part == ProbeLines )
    {
        ps.set( mb + mix::KICK_MUTE, 1.0f );
        ps.set( mb + mix::BASS_MUTE, 1.0f );
        ps.set( mb + mix::PERC_MUTE, 1.0f );
    }
    TempoMap tm;
    tm.setConstant( plan.m_bpm );

    std::vector<ControlEvent> controls;
    TrackPlan                 neutral = plan;
    neutral.m_masterGainDb = masterGainDb;
    // The presence probe keeps the parts' level corrections: the balance it measures is the one they make.
    if ( !mixLike )
    {
        neutral.m_gainDb = 0.0f;
        neutral.m_presenceGainDb = 0.0f;
        for ( float &g : neutral.m_partGainDb ) g = 0.0f;
    }
    trackStartControls( p, neutral, 0.0, controls );
    arcControls( p, neutral, 0, 0.0, false, controls );

    const int root = bassRootNote( plan.m_key, p.getInt( p.base( ModuleE::COMPOSE ) + compose::BASS_REGISTER ) );
    const BassPatternDef  &pat = BassPatterns[plan.m_primaryPattern];
    std::vector<NoteEvent> notes;
    for ( int beat = 0; beat < bars * BeatsPerBar; ++beat )
    {
        NoteEvent k;
        k.m_beat = beat;
        k.m_part = PartE::KICK;
        k.m_velocity = 127;
        notes.push_back( k );
        for ( int s = 0; s < pat.m_count; ++s )
        {
            const double next = s + 1 < pat.m_count ? pat.m_pos[s + 1] : 1.0;
            NoteEvent    n;
            n.m_beat = beat + pat.m_pos[s];
            n.m_length = static_cast<float>( ( next - pat.m_pos[s] ) * plan.m_gate );
            n.m_part = PartE::BASS;
            // The probe plays the track's own bass line, because a learned phrase is not the root
            // over and over: a bar of octaves is measurably quieter through the same ladder than a
            // bar of roots, and the level match between tracks would inherit that error. With the
            // pattern generator the phrase is absent and this is the root, as it always was.
            const int rel =
                plan.m_bassNeural
                    ? plan.m_bassRel[0][( ( beat / BeatsPerBar % BassPhraseBars ) * BeatsPerBar + beat % BeatsPerBar ) *
                                            3 +
                                        s]
                    : 0;   // beat runs over the whole probe, so beat/4 is its bar
            n.m_pitch = static_cast<uint8_t>( std::clamp( root + rel, 0, 127 ) );
            n.m_velocity = 110;
            notes.push_back( n );
        }
    }
    // Which bar of the track each probe bar is: 49 and 50 (the percussion's layers are in) for the foundation
    // and the parts, the middle bar of the block at each fifth of the track for the whole mix.
    // Four contiguous windows of four bars, spread evenly over the track. Contiguous, because a pad
    // holds its chord over several bars and a reverb tail runs on: single bars cut from the middle of a
    // breakdown read far quieter than the same bar does in the track.
    auto sourceBar = [&]( int b )
    {
        if ( part == ProbeAudible )
        {
            int d1 = -1, d2 = -1;
            for ( int i = 0; i < plan.m_form.m_count; ++i )
                if ( plan.m_form.m_section[i].m_type == SectionTypeE::DROP )
                {
                    if ( d1 < 0 ) d1 = i;
                    d2 = i;
                }
            const Section &s = plan.m_form.m_section[std::max( 0, b < 8 ? d1 : d2 )];
            const int      in =
                8 + ( b < 8 ? b : b - 8 );   // from the second group on: the drop's first bars carry the impact
            return s.m_startBar + std::min( s.m_bars - 1, in );
        }
        if ( presence )
        {
            // Four bars of drop 1 and four of drop 2 (the last drop, where a form has only one): the fifth bar
            // of each of their first four eight-bar groups. One per group, because the stab and the counter
            // take whole groups by turns (Form.cpp), and two bars from the middle heard one of them or neither
            // -- on the calibration tracks the probe then read the brightest tracks 2 dB low.
            int d1 = -1, d2 = -1;
            for ( int i = 0; i < plan.m_form.m_count; ++i )
                if ( plan.m_form.m_section[i].m_type == SectionTypeE::DROP )
                {
                    if ( d1 < 0 ) d1 = i;
                    d2 = i;
                }
            const Section &s = plan.m_form.m_section[std::max( 0, b < 4 ? d1 : d2 )];
            return s.m_startBar + std::min( s.m_bars - 1, 8 * ( b % 4 ) + 4 );
        }
        if ( part != -2 ) return 49 + b;
        // The first window starts on bar 0 and the last ends on the last bar, so that the intro and
        // the outro count: they are a tenth of the track and quieter than everything else, and a probe
        // that skipped them aimed about 1 LU too high.
        const int window = ( ( b / 4 ) * ( plan.m_bars - 4 ) / 3 ) / 4 * 4;
        return window + b % 4;
    };
    // The bar plan of a probe bar: the real instrumentation matrix for the whole mix, everything the
    // track has for a single part or the foundation (those measure a sound, not an arrangement).
    auto probeBar = [&]( int source )
    {
        if ( !mixLike )
        {
            BarPlan bp = allPartsBar( plan.m_melody );
            bp.m_percLayers = plan.m_perc.m_layers;
            // A part's own probe plays that part whether or not the track uses it (19.09.2026): every
            // track has the material of every part, and the first track is the reference the later
            // ones are matched against -- a first track without a stab left every later stab unmatched.
            if ( part >= 0 ) bp.m_parts = bp.m_partsNext = partBit( static_cast<MelodyPartE>( part ) );
            return bp;
        }
        return planBar( plan.m_form, availabilityOf( plan ), plan.m_sectionSeed, source );
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
    int coreLayers = plan.m_perc.m_layers;
    int coreAt = -1;
    for ( int pass = 0; pass < 2 && coreAt < 0; ++pass )
        for ( int i = 0; i < plan.m_form.m_count && coreAt < 0; ++i )
        {
            const SectionTypeE st = plan.m_form.m_section[i].m_type;
            if ( st == SectionTypeE::DROP || ( pass == 1 && st == SectionTypeE::GROOVE ) ) coreAt = i;
        }
    if ( coreAt >= 0 )
    {
        const Section &core = plan.m_form.m_section[coreAt];
        coreLayers = planBar( plan.m_form, availabilityOf( plan ), plan.m_sectionSeed,
                              core.m_startBar + std::min( 4, core.m_bars - 1 ) )
                         .m_percLayers;
    }
    for ( int b = 0; b < bars; ++b )
    {
        BarPlan bp = probeBar( sourceBar( b ) );
        if ( part == -1 ) bp.m_percLayers = std::min( bp.m_percLayers, coreLayers );
        // The whole-mix probe has to hear the loudness side of the energy arc as well, or Auto Gain
        // would aim at a track that is louder than the one the form really plays (measured: 1.3 LU).
        if ( part == -2 )
        {
            const Section   &sec = plan.m_form.m_section[std::clamp( bp.m_index, 0, MaxSections - 1 )];
            const ParamDesc &g = p.desc( mb + mix::TRACK_GAIN );
            ControlEvent     c;
            c.m_beat = static_cast<double>( b ) * BeatsPerBar;
            c.m_param = static_cast<int16_t>( mb + mix::TRACK_GAIN );
            c.m_kind = ControlEvent::KindE::OFFSET;
            // With the climax trim at the probe bar's place in its section (Form.h, Drop1HoldDb).
            const double u = sec.m_bars > 1 ? static_cast<double>( sourceBar( b ) - sec.m_startBar ) / sec.m_bars : 0.0;
            c.m_value = ( plan.m_gainDb + energyGainDb( 0.5f * ( sec.m_energy + sec.m_energyTo ) ) +
                          sectionTrimDb( plan.m_form, bp.m_index, u ) ) /
                        ( g.m_maxValue - g.m_minValue );
            controls.push_back( c );
        }
        PercBarSpec spec;
        spec.m_layers = bp.m_percLayers;
        spec.m_fills = false;
        spec.m_hatsDense = bp.m_hatsDense;
        spec.m_rollBar = bp.m_rollBar;
        spec.m_rollBars = bp.m_rollBars;
        spec.m_pdb = bp.m_pdb;
        spec.m_cutBeats = bp.m_cutBeats;
        spec.m_quietHats = bp.m_quietHats;
        spec.m_shaker = bp.m_shaker;
        spec.m_offbeatHat = bp.m_offbeatHat;
        spec.m_hatLevel = bp.m_hatLevel;
        spec.m_openHats = bp.m_openHats;
        spec.m_ride = bp.m_ride;
        composePercBar( p, plan.m_perc, plan.m_percSeed, b, sourceBar( b ), plan.m_bpm, plan.m_key, plan.m_scale, spec,
                        notes );
    }
    // The melodic parts: everything the track has for their own measurement, as the form plays them
    // for the whole mix.
    if ( part >= 0 || mixLike )
    {
        std::vector<NoteEvent> mel;
        for ( int b = 0; b < bars; ++b )
        {
            const BarPlan bp = probeBar( sourceBar( b ) );
            // The drone plays one held chord per run (MelodyContext::droneBars), which a probe bar
            // taken out of the middle of a run would never start: the part probe holds it over its two
            // bars in the upper octave, the mix probe over each four-bar window in the octave the form has
            // there (19.09.2026).
            MelodyContext ctx;
            if ( mixLike )
            {
                ctx = melodyContext( plan, bp, sourceBar( b ) );
                if ( b % 4 == 0 && ctx.m_droneBars == 0 && ( bp.m_parts & partBit( MelodyPartE::DRONE ) ) != 0 )
                {
                    ctx.m_droneBars = 4;
                    ctx.m_droneLow = droneLowAt( plan, sourceBar( b ) );
                }
            }
            else if ( b == 0 )
            {
                ctx.m_droneBars = bars;
            }
            composeMelodyBar( p, plan.m_melody, b, mixLike ? sourceBar( b ) : 48 + b, plan.m_scale, bp, mel, ctx );
        }
        for ( const NoteEvent &n : mel )
            if ( mixLike || n.m_part == melodyScorePart( static_cast<MelodyPartE>( part ) ) ) notes.push_back( n );
    }
    // The presence probe hears each drop's own section controls -- the energy's filter arc, the section's drawn
    // wobble, drop 2's lift (Form.h, ClimaxOpen) -- at once rather than as the ramps a whole section plays them
    // over; without them the probe read the brighter tracks up to 2 dB low against the render (calibration, above
    // matchPresence). Only events inside the probe, in time order behind the track-start controls at beat 0.
    if ( presence )
    {
        std::vector<ControlEvent> sc;
        for ( int b = 0; b < bars; b += 4 )
            sectionControls( p, neutral, probeBar( sourceBar( b ) ), static_cast<double>( b ) * BeatsPerBar, sc );
        std::vector<ControlEvent> keep;
        for ( ControlEvent c : sc )
        {
            if ( c.m_beat >= static_cast<double>( bars ) * BeatsPerBar ) continue;
            c.m_length = 0.0f;
            keep.push_back( c );
        }
        std::stable_sort( keep.begin(), keep.end(),
                          []( const ControlEvent &a, const ControlEvent &b ) { return a.m_beat < b.m_beat; } );
        for ( const ControlEvent &c : keep ) controls.push_back( c );
    }
    // One ring, so one order: the engine plays from the head of the ring.
    std::stable_sort( notes.begin(), notes.end(), noteLess );
    const int total = static_cast<int>( tm.secondsAt( bars * BeatsPerBar ) * sr );

    // The cache key: everything the render below depends on (Probe.h). Field by field, so that padding
    // bytes never enter it; with the counts, so that a note cannot pass for a control.
    // The audibility probe returns a reading per part, more than the cache's three values: never cached.
    const bool cached = probe::cacheEnabled() && audible == nullptr;
    probe::Key key;
    if ( cached )
    {
        // The shared data is hashed as it is loaded *now*, so it has to be loaded before the key is made and
        // not by the engine after it: "not loaded yet" and "the file is missing" would otherwise be one key
        // for two different renders. A flag test when it has happened (and it has, before any worker runs).
        probe::warmSharedData();
        probe::Hasher h;
        h.text( "phos probe 1" );   // the layout of this key
        h.text( probe::buildId() );
        h.add( probe::sharedDataId() );
        h.add( sr );
        h.add( static_cast<int32_t>( 512 ) );
        h.add( static_cast<int32_t>( total ) );
        h.add( static_cast<uint8_t>( bands != nullptr ? 1 : 0 ) );
        h.add( plan.m_bpm );
        h.add( static_cast<int32_t>( ps.count() ) );
        for ( int i = 0; i < ps.count(); ++i ) h.add( ps.get( i ) );
        h.add( static_cast<uint64_t>( controls.size() ) );
        for ( const ControlEvent &c : controls )
        {
            h.add( c.m_beat );
            h.add( c.m_length );
            h.add( c.m_value );
            h.add( c.m_param );
            h.add( c.m_kind );
        }
        h.add( static_cast<uint64_t>( notes.size() ) );
        for ( const NoteEvent &n : notes )
        {
            h.add( n.m_beat );
            h.add( n.m_length );
            h.add( n.m_part );
            h.add( n.m_lane );
            h.add( n.m_pitch );
            h.add( n.m_velocity );
            h.add( n.m_flags );
        }
        key = h.finish();
        double v[3];
        if ( probe::cacheLookup( key, v ) )
        {
            if ( bands != nullptr )
            {
                bands[0] = v[1];
                bands[1] = v[2];
            }
            return v[0];
        }
    }

    auto engine = std::make_unique<Engine>();
    engine->prepare( sr, 512 );
    engine->params().copyValuesFrom( ps );
    engine->setTempoMap( tm );
    for ( const ControlEvent &c : controls ) engine->pushControl( c );
    for ( const NoteEvent &n : notes ) engine->pushEvent( n );
    LoudnessMeter meter;
    meter.prepare( sr );
    std::vector<float> L( 512 ), R( 512 );
    // Band powers for the presence match: 1.5..6 kHz and 40..140 Hz, each a fourth-order band (two
    // second-order high passes and two low passes, Butterworth Q), the bands of Tools/metrics.py.
    Svf bandFilter[2][2][4];
    for ( int c = 0; c < 2; ++c )
        for ( int k = 0; k < 2; ++k )
        {
            bandFilter[0][c][k].setQ( 1500.0f, 0.7071f, static_cast<float>( sr ) );
            bandFilter[0][c][2 + k].setQ( 6000.0f, 0.7071f, static_cast<float>( sr ) );
            bandFilter[1][c][k].setQ( 40.0f, 0.7071f, static_cast<float>( sr ) );
            bandFilter[1][c][2 + k].setQ( 140.0f, 0.7071f, static_cast<float>( sr ) );
        }
    double bandSum[2] = {};
    // The stems for the audibility meter (Engine.h, StemTap; Audibility.h), only when they are asked for.
    std::vector<std::vector<float>>  stemL, stemR;
    StemTap                          tap;
    std::unique_ptr<AudibilityMeter> aud;
    if ( audible != nullptr )
    {
        stemL.assign( NumStems, std::vector<float>( 512 ) );
        stemR.assign( NumStems, std::vector<float>( 512 ) );
        for ( int s = 0; s < NumStems; ++s )
        {
            tap.m_L[s] = stemL[static_cast<size_t>( s )].data();
            tap.m_R[s] = stemR[static_cast<size_t>( s )].data();
        }
        engine->setStemTap( &tap );
        aud = std::make_unique<AudibilityMeter>( NumStems, sr );
    }
    for ( int done = 0; done < total; done += 512 )
    {
        // A background copy that is being torn down (setAbortFlag): stop, and cache nothing.
        if ( m_abort != nullptr && m_abort->load( std::memory_order_relaxed ) ) return 0.0;
        const int n = std::min( 512, total - done );
        engine->process( L.data(), R.data(), n );
        meter.process( L.data(), R.data(), n );
        if ( aud != nullptr )
        {
            const float *sl[NumStems];
            const float *sr2[NumStems];
            for ( int s = 0; s < NumStems; ++s )
            {
                sl[s] = stemL[static_cast<size_t>( s )].data();
                sr2[s] = stemR[static_cast<size_t>( s )].data();
            }
            aud->add( sl, sr2, n );
        }
        if ( bands == nullptr ) continue;
        for ( int c = 0; c < 2; ++c )
        {
            const float *x = c == 0 ? L.data() : R.data();
            for ( int i = 0; i < n; ++i )
                for ( int band = 0; band < 2; ++band )
                {
                    float y = x[i], lp, bp, hp;
                    for ( int k = 0; k < 2; ++k )
                    {
                        bandFilter[band][c][k].tick( y, lp, bp, hp );
                        y = hp;
                    }
                    for ( int k = 2; k < 4; ++k ) y = bandFilter[band][c][k].lp( y );
                    bandSum[band] += static_cast<double>( y ) * y;
                }
        }
    }
    if ( bands != nullptr )
    {
        bands[0] = bandSum[0];
        bands[1] = bandSum[1];
    }
    if ( aud != nullptr )
    {
        engine->setStemTap( nullptr );
        for ( int k = 0; k < MelodyParts; ++k )
            audible[k] = aud->read( static_cast<int>( melodyScorePart( static_cast<MelodyPartE>( k ) ) ) ).m_inMix;
    }
    const double integrated = meter.read().m_integrated;
    if ( cached )
    {
        const double v[3] = { integrated, bandSum[0], bandSum[1] };
        probe::cacheStore( key, v );
    }
    return integrated;
}

}   // namespace phos
