/**
 * @file Perc.cpp
 * @brief Percussion kit: coefficients, hits and rendering.
 * @author Rene Weller
 */
#include "phos/Perc.h"
#include "phos/Harmony.h"
#include "phos/Util.h"
#include <algorithm>
#include <cmath>

namespace phos
{

namespace
{

constexpr double PiD = 3.141592653589793;      ///< pi
constexpr double Ln1000 = 6.907755278982137;   ///< ln 1000: a decay of 60 dB

/** @brief Mode frequency ratios: membrane (Bessel zeros), free-free bar, loaded membrane (tabla). */
constexpr double ModeRatios[3][PercModes] = {
    { 1.0, 1.593, 2.136, 2.295 },
    { 1.0, 2.756, 5.404, 8.933 },
    { 1.0, 2.0, 3.0, 4.0 },
};
/** @brief Strike amplitudes of the modes, before normalisation. */
constexpr double ModeAmps[3][PercModes] = {
    { 1.0, 0.7, 0.5, 0.4 },
    { 1.0, 0.5, 0.25, 0.12 },
    { 1.0, 0.5, 0.33, 0.25 },
};
/** @brief TR-808 cymbal oscillator frequencies. */
constexpr double MetalHz[PercMetalOsc] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
/** @brief How far a metal lane's noise pulls its squares down: at noise 1 they keep 1 - 0.8 = 0.2 (-14 dB). */
constexpr float MetalNoiseTrade = 0.8f;

/**
 * @brief The per-sample factor of a decay of 60 dB in @p seconds at rate @p sr.
 * @param[in] seconds  the time to fall by 60 dB
 * @param[in] sr       the sample rate in Hz
 * @return the per-sample factor
 */
double decayFactor( double seconds, double sr )
{
    return std::exp( -Ln1000 / ( std::max( seconds, 1.0e-4 ) * sr ) );
}

/**
 * @name The auto-pan (16.09.2026)
 * @{ */
/** @brief sqrt(2): the swing amplitude that keeps E[p^2] at p0^2 together with sqrt(1 - D^2) (Perc.h). */
constexpr double PanRms = 1.4142135623730951;
/**
 * @brief Largest rotation angle the kernel's Taylor series may be asked for.
 *
 * cos and sin are taken to the w^6 and w^7 terms there and the pair is renormalised by one Newton
 * step afterwards, so the truncation survives only as an angle error: at |w| = 1.6 the first dropped
 * term is w^8/8! = 1.1e-3 rad, which is 0.01 dB on a gain. The angle a lane can ask for is
 * (|panA| + |panB|) and stays under 1.6 for every position and depth the field allows (Perc.h); the
 * clamp is a guard on a hand-edited preset, not a working limit.
 */
constexpr double PanMaxAngle = 1.6;
/** @} */

}   // namespace

double PercKit::tuneToScale( double hz, int keyRoot, int scale )
{
    const double note = 69.0 + 12.0 * std::log2( hz / 440.0 );
    const int    base = static_cast<int>( std::floor( note ) );
    double       best = hz, bestDist = 1e9;
    for ( int n = base - 2; n <= base + 3; ++n )
    {
        if ( !inScale( scale, n - keyRoot ) ) continue;
        const double d = std::fabs( n - note );
        if ( d < bestDist )
        {
            bestDist = d;
            best = midiToHz( n );
        }
    }
    return best;
}

double PercKit::panAngle( int lane ) const
{
    if ( lane < 0 || lane >= PercLanes ) return 0.0;
    return static_cast<double>( m_c.m_panA[lane] ) * static_cast<double>( m_s.m_pr[lane] ) +
           static_cast<double>( m_c.m_panB[lane] );
}

double PercKit::panPosition( int lane ) const
{
    if ( lane < 0 || lane >= PercLanes ) return 0.0;
    return std::clamp( static_cast<double>( m_values[lane][perc::PAN] ), -1.0, 1.0 ) + panAngle( lane ) * 4.0 / PiD;
}

double PercKit::panSwing( int lane ) const
{
    return ( lane < 0 || lane >= PercLanes ) ? 0.0 : static_cast<double>( m_c.m_panA[lane] );
}

int PercKit::panGroup( int lane ) const
{
    if ( lane < 0 || lane >= PercLanes ) return 0;
    const double p0 = static_cast<double>( m_values[lane][perc::PAN] );
    if ( m_c.m_panA[lane] == 0.0f || p0 == 0.0 ) return 0;
    return ( static_cast<double>( m_c.m_panA[lane] ) * p0 > 0.0 ) ? +1 : -1;
}

void PercKit::setTempo( double bpm )
{
    const double b = std::clamp( bpm, 20.0, 400.0 );
    if ( b == m_bpm ) return;
    m_bpm = b;
    // Only the phasor's step depends on the tempo. The engine calls this at every 32-sample chunk,
    // and during a tempo ramp between tracks the value changes at every one of them, so the full
    // coefficient set (filters, modes, the pan groups) is not recomputed here.
    for ( int l = 0; l < PercLanes; ++l )
        if ( m_valid[l] ) updatePanRate( l );
}

/**
 * @brief The phase group of each role: +1 keeps a lane's swing outwards-first, -1 turns it round.
 *
 * 19.09.2026 (round "voices", a loose end of the arrangement dynamics of 16.09.2026). Until then the
 * groups were a greedy two-way number partition of the lanes' knob levels (Graham 1969), recomputed
 * whenever any lane changed. A partition of *levels* flips discontinuously: with the closed hat at 4 dB
 * instead of 5, or the shaker 4 dB lower, the greedy order changed, the shaker and the conga swapped
 * groups, and the 85 ms level-difference reduction the auto-pan exists for fell from about 2 dB to
 * 0.17 (testArrangeDynamics). The coordinator then had to correct the air band with mix.perc_level
 * instead of the hat, because the hat could not be touched.
 *
 * The groups are now a property of the role -- what a lane *plays* -- and do not depend on any level:
 * the table is exactly the greedy answer at the default kit, so the default kit moves as it did,
 * sample for sample, and a level change moves the balance continuously instead of re-dealing the kit.
 * The pairing it encodes is the musical one: the closed hat against the ride and the shaker (the three
 * steady top-end streams), the open hat with it, conga and tom (the hand drums) with the hat, crash,
 * rim, zap and blip against it. Clap and snare stand in the centre and do not swing.
 */
constexpr int8_t RolePanGroup[NumPercRoles] = {
    +1,   // Closed Hat
    +1,   // Open Hat
    -1,   // Ride
    -1,   // Crash
    +1,   // Clap (centred: no swing)
    +1,   // Snare (centred: no swing)
    -1,   // Rim
    -1,   // Shaker
    +1,   // Tom
    +1,   // Conga
    -1,   // Zap
    -1,   // Blip
};

/**
 * @brief Signs the auto-pan swings of the lanes by their roles' phase groups (RolePanGroup).
 *
 * What the 85 ms level difference reads is the power-weighted mean position of whatever sounds in
 * the window (Perc.h). The swing of a lane carries the sign of its own position, so that "in phase"
 * means "swings outwards first", times its role's group. Runs over all twelve lanes whenever any one
 * of them changes, which happens on a parameter change and not per sample; nothing in it depends on a
 * level, so no level change can re-deal the groups.
 */
void PercKit::assignPanGroups()
{
    for ( int l = 0; l < PercLanes; ++l )
    {
        const double dir = m_values[l][perc::PAN] < 0.0f ? -1.0 : 1.0;
        const int role = std::clamp( static_cast<int>( std::lround( m_values[l][perc::ROLE] ) ), 0, NumPercRoles - 1 );
        m_c.m_panA[l] = static_cast<float>( std::fabs( static_cast<double>( m_c.m_panA[l] ) ) * dir *
                                            static_cast<double>( RolePanGroup[role] ) );
    }
    m_panning = false;
    for ( int l = 0; l < PercLanes; ++l ) m_panning = m_panning || m_c.m_panA[l] != 0.0f || m_c.m_panB[l] != 0.0f;
}
void PercKit::prepare( double sampleRate )
{
    m_sr = sampleRate;
    const size_t n = static_cast<size_t>( MaxBlock * PercLaneSlots );
    m_noise.assign( n, 0.0f );
    m_reset.assign( n, 0.0f );
    m_dNoise.assign( n, 1.0f );
    m_outL.assign( n, 0.0f );
    m_outR.assign( n, 0.0f );
    m_chokeFactor = static_cast<float>( decayFactor( 0.008, m_sr ) );
    for ( int l = 0; l < PercLanes; ++l )
    {
        m_valid[l] = false;
        m_shiftMul[l] = 1.0;
    }
    reset();
}

void PercKit::reset()
{
    m_s = PercState{};
    for ( int l = 0; l < PercLaneSlots; ++l )
    {
        m_s.m_cr[l] = 1.0f;
        m_s.m_mr[l] = 1.0f;
        m_s.m_choke[l] = 1.0f;
        m_c.m_chokeD[l] = 1.0f;
        m_c.m_dP[l] = 1.0f;
        m_c.m_dA[l] = 1.0f;
    }
    for ( int l = 0; l < PercLaneSlots; ++l )
    {
        // Every auto-pan phasor starts at angle zero: the two phase groups differ by the sign of
        // panA, not by a phase offset (Perc.h), so one starting point serves the whole kit and the
        // movement is reproducible from a reset without carrying a phase table.
        m_s.m_pr[l] = 1.0f;
        m_s.m_pi[l] = 0.0f;
        m_c.m_panC[l] = 1.0f;
        m_c.m_panS[l] = 0.0f;
    }
    for ( int l = 0; l < PercLanes; ++l )
    {
        m_noiseRng[l].seed( 0x5045524300ull + static_cast<uint64_t>( l ) );
        m_burstsLeft[l] = 0;
        m_burstTimer[l] = 0.0;
    }
}

void PercKit::update( int lane, const float *v, int keyRoot, int scale )
{
    bool same = m_valid[lane] && keyRoot == m_keyRoot[lane] && scale == m_scale[lane];
    for ( int i = 0; same && i < perc::COUNT; ++i ) same = m_values[lane][i] == v[i];
    if ( same ) return;
    std::copy( v, v + perc::COUNT, m_values[lane] );
    m_keyRoot[lane] = keyRoot;
    m_scale[lane] = scale;
    m_valid[lane] = true;
    computeCoefs( lane );
}

void PercKit::computeCoefs( int l )
{
    const float *v = m_values[l];
    const bool   active = v[perc::ACTIVE] >= 0.5f;
    m_role[l] = static_cast<int>( std::lround( v[perc::ROLE] ) );
    m_engine[l] = static_cast<int>( std::lround( v[perc::ENGINE] ) );
    m_choke[l] = static_cast<int>( std::lround( v[perc::CHOKE] ) );
    const PercEngineE engine = static_cast<PercEngineE>( m_engine[l] );

    double hz = v[perc::PITCH];
    if ( v[perc::TUNE] >= 0.5f ) hz = tuneToScale( hz, m_keyRoot[l], m_scale[l] );
    m_tunedHz[l] = hz;
    const double f = hz * m_shiftMul[l];

    // Sources.
    m_c.m_wTone[l] = ( engine == PercEngineE::TONE || engine == PercEngineE::FM ) ? 1.0f : 0.0f;
    m_c.m_wModal[l] = engine == PercEngineE::MODAL ? 1.0f : 0.0f;
    // Since 25.09.2026 the noise of a metal lane trades against its squares instead of lying on top of them:
    // six summed squares are six steady tones, and with a fixed weight no amount of noise buried them (the ride
    // rang 19 dB over its neighbourhood at 2.9 kHz, which the user heard as feedback; at 0.6 still 8 dB).
    m_c.m_wMetal[l] = engine == PercEngineE::METAL ? 1.0f - MetalNoiseTrade * v[perc::NOISE] : 0.0f;
    m_c.m_wNoise[l] = engine == PercEngineE::NOISE ? 1.0f : v[perc::NOISE];

    const double w0 = std::min( 2.0 * PiD * f / m_sr, 1.2 );
    m_c.m_w0[l] = static_cast<float>( w0 );
    m_c.m_pAmt[l] = v[perc::PITCH_AMOUNT] - 1.0f;
    m_c.m_dP[l] = static_cast<float>( std::exp( -1.0 / ( std::max( 0.5e-3, v[perc::PITCH_DECAY] * 0.001 ) * m_sr ) ) );
    m_c.m_fmIdx[l] = engine == PercEngineE::FM ? v[perc::FM_INDEX] : 0.0f;
    const double wm = std::min( w0 * v[perc::FM_RATIO], 0.95 * PiD );
    m_c.m_mc[l] = static_cast<float>( std::cos( wm ) );
    m_c.m_ms[l] = static_cast<float>( std::sin( wm ) );
    const double decay = v[perc::DECAY] * 0.001;
    m_c.m_dA[l] = static_cast<float>( decayFactor( decay, m_sr ) );

    // Modes: each higher mode decays faster by ratio^(-1.5 damping); amplitudes normalised to sum 1.
    const int set = std::clamp( static_cast<int>( std::lround( v[perc::MODE_SET] ) ), 0, 2 );
    double    ampSum = 0.0;
    for ( int m = 0; m < PercModes; ++m ) ampSum += ModeAmps[set][m];
    for ( int m = 0; m < PercModes; ++m )
    {
        const double wk = 2.0 * PiD * f * ModeRatios[set][m] / m_sr;
        const bool   audible = wk < 0.95 * PiD;
        const double tau = decay * std::pow( ModeRatios[set][m], -1.5 * v[perc::MODE_DAMP] );
        const double r = audible ? decayFactor( tau, m_sr ) : 0.0;
        m_modeW[m][l] = wk;
        m_modeR[m][l] = r;
        m_modeAmp[m][l] = audible ? static_cast<float>( ModeAmps[set][m] / ampSum ) : 0.0f;
        m_c.m_modeC[m][l] = static_cast<float>( r * std::cos( wk ) );
        m_c.m_modeS[m][l] = static_cast<float>( r * std::sin( wk ) );
    }

    // Metal.
    const double scale = v[perc::METAL_SCALE];
    for ( int j = 0; j < PercMetalOsc; ++j )
    {
        const double dt = std::min( MetalHz[j] * scale / m_sr, 0.45 );
        m_c.m_dt[j][l] = static_cast<float>( dt );
        m_c.m_inv[j][l] = static_cast<float>( 1.0 / dt );
    }

    // Noise envelopes: the tail, and the fast decay inside a burst (about 30 dB per burst spacing).
    const double spacing = v[perc::BURST_SPACING] * 0.001;
    m_burstSpacing[l] = spacing * m_sr;
    m_noiseTail[l] = static_cast<float>( decayFactor( v[perc::NOISE_DECAY] * 0.001, m_sr ) );
    m_noiseFast[l] = static_cast<float>( std::exp( std::log( 0.03 ) / std::max( 1.0, spacing * m_sr ) ) );

    // Main filter.
    const int    mode = std::clamp( static_cast<int>( std::lround( v[perc::FILTER] ) ), 0, 2 );
    const double damping = 2.0 - 1.9 * std::clamp( static_cast<double>( v[perc::RESONANCE] ), 0.0, 1.0 );
    svfCoefs( v[perc::CUTOFF], damping, m_sr, m_c.m_a1[l], m_c.m_a2[l], m_c.m_a3[l] );
    m_c.m_k[l] = static_cast<float>( damping );
    m_c.m_mLp[l] = mode == 0 ? 1.0f : 0.0f;
    m_c.m_mBp[l] = mode == 1 ? 1.0f : 0.0f;
    m_c.m_mHp[l] = mode == 2 ? 1.0f : 0.0f;
    // Low cut, never below 150 Hz. With cut_track it follows the hit's pitch shift: m_shiftMul is
    // 2^(semitones/12), so the corner moves by 2^(cut_track * semitones/12).
    //
    // The useful setting is *above* one, and the reason is worth writing down. At exactly 1 the corner
    // and the note move together, which transposes the lane and thins it not at all -- a shifted copy
    // of the same timbre. Only a corner that climbs faster than the note eats into the body, which is
    // what a buildup roll does: at 2 an octave of pitch puts the high pass two octaves up, the snare's
    // fundamental ends below its own filter, and what is left is the noise. That is the "thin it while
    // it rises" of the review, and it costs no new signal path.
    const double cutTrack = std::clamp( static_cast<double>( v[perc::CUT_TRACK] ), 0.0, 2.0 );
    const double lowCut =
        static_cast<double>( v[perc::LOW_CUT] ) * ( cutTrack > 0.0 ? std::pow( m_shiftMul[l], cutTrack ) : 1.0 );
    svfCoefs( std::max( 150.0, lowCut ), std::sqrt( 2.0 ), m_sr, m_c.m_c1[l], m_c.m_c2[l], m_c.m_c3[l] );

    // Drive, level, pan (constant power, unity in the centre).
    const double g = 0.1 + 6.0 * v[perc::DRIVE];
    m_c.m_drvG[l] = static_cast<float>( g );
    m_c.m_drvN[l] = static_cast<float>( 1.0 / g );
    const double level = active ? std::pow( 10.0, v[perc::LEVEL] / 20.0 ) : 0.0;
    const double p0 = std::clamp( static_cast<double>( v[perc::PAN] ), -1.0, 1.0 );
    const double theta = ( p0 + 1.0 ) * PiD / 4.0;
    m_c.m_gL[l] = static_cast<float>( level * std::cos( theta ) * std::sqrt( 2.0 ) );
    m_c.m_gR[l] = static_cast<float>( level * std::sin( theta ) * std::sqrt( 2.0 ) );

    // The auto-pan (Perc.h). p(t) = p0 [ sqrt(1 - D^2) + sqrt(2) D cos(phase) ], written as the angle
    // the gain pair is turned by: d(t) = (p(t) - p0) pi/4 = panA cos(phase) + panB.
    const double depth = std::clamp( static_cast<double>( v[perc::PAN_DEPTH] ), 0.0, 1.0 );
    double       stand = std::sqrt( std::max( 0.0, 1.0 - depth * depth ) ), swing = PanRms * depth;
    // Keep the swing inside the field. |p0| (stand + swing) can exceed 1 at an intermediate depth
    // even when it does not at D = 1 (the sum peaks at sqrt(3) around D = 0.82), so the guard reads
    // the actual depth. Scaling both parts keeps the shape of the movement and never folds a
    // position past the edge, where the panner would invert a channel. No default kit reaches it:
    // the widest lane stands at 0.60 and asks for 0.849 at D = 1.
    const double reach = std::fabs( p0 ) * ( stand + swing );
    if ( reach > 1.0 )
    {
        stand /= reach;
        swing /= reach;
    }
    double a = p0 * swing * PiD / 4.0;
    double b = p0 * ( stand - 1.0 ) * PiD / 4.0;
    // And inside the angle the kernel's Taylor series is good for.
    const double span = std::fabs( a ) + std::fabs( b );
    if ( span > PanMaxAngle )
    {
        const double k = PanMaxAngle / span;
        a *= k;
        b *= k;
    }
    m_c.m_panA[l] = static_cast<float>( a );
    m_c.m_panB[l] = static_cast<float>( b );
    updatePanRate( l );
    assignPanGroups();
}

void PercKit::updatePanRate( int l )
{
    const double bars = std::clamp( static_cast<double>( m_values[l][perc::PAN_BARS] ), 0.0625, 64.0 );
    const double w = 2.0 * PiD / std::max( 1.0, bars * 4.0 * 60.0 / m_bpm * m_sr );
    m_c.m_panC[l] = static_cast<float>( std::cos( w ) );
    m_c.m_panS[l] = static_cast<float>( std::sin( w ) );
}

void PercKit::trigger( int l, float velocity, int shift, double late )
{
    if ( l < 0 || l >= PercLanes || !m_valid[l] ) return;
    const double mul = std::pow( 2.0, shift / 12.0 );
    if ( mul != m_shiftMul[l] )
    {
        m_shiftMul[l] = mul;
        computeCoefs( l );
    }
    const double vel = clampv( static_cast<double>( velocity ), 0.0, 1.0 );

    m_s.m_envA[l] = static_cast<float>( vel * std::pow( static_cast<double>( m_c.m_dA[l] ), late ) );
    m_s.m_envP[l] = static_cast<float>( std::pow( static_cast<double>( m_c.m_dP[l] ), late ) );
    const int bursts = static_cast<int>( std::lround( m_values[l][perc::BURSTS] ) );
    m_burstsLeft[l] = bursts - 1;
    m_burstTimer[l] = m_burstSpacing[l] - late;
    m_burstVel[l] = static_cast<float>( vel );
    const double nf = bursts > 1 ? m_noiseFast[l] : m_noiseTail[l];
    m_s.m_envN[l] = static_cast<float>( vel * std::pow( nf, late ) );

    // The carrier starts at phase zero, `late` samples ago at its starting frequency.
    const double wStart = std::min( static_cast<double>( m_c.m_w0[l] ) * ( 1.0 + m_c.m_pAmt[l] ), 1.2 );
    m_s.m_cr[l] = static_cast<float>( std::cos( wStart * late ) );
    m_s.m_ci[l] = static_cast<float>( std::sin( wStart * late ) );
    m_s.m_mr[l] = static_cast<float>( std::cos( std::atan2( m_c.m_ms[l], m_c.m_mc[l] ) * late ) );
    m_s.m_mi[l] = static_cast<float>( std::sin( std::atan2( m_c.m_ms[l], m_c.m_mc[l] ) * late ) );
    // Modes: energy added to the ring, so a roll sums.
    for ( int m = 0; m < PercModes; ++m )
    {
        const double a = m_modeAmp[m][l] * vel * std::pow( m_modeR[m][l], late );
        m_s.m_zr[m][l] += static_cast<float>( a * std::cos( m_modeW[m][l] * late ) );
        m_s.m_zi[m][l] += static_cast<float>( a * std::sin( m_modeW[m][l] * late ) );
    }
    // Chokes: this lane opens, the others in its group close within 8 ms.
    m_s.m_choke[l] = 1.0f;
    m_c.m_chokeD[l] = 1.0f;
    if ( m_choke[l] > 0 )
    {
        for ( int j = 0; j < PercLanes; ++j )
        {
            if ( j != l && m_choke[j] == m_choke[l] ) m_c.m_chokeD[j] = m_chokeFactor;
        }
    }
}

template <class VT>
void PercKit::processWith( float *L, float *R, int total )
{
    const int width = laneWidth<VT>();
    int       done = 0;
    while ( done < total )
    {
        const int n = std::min( MaxBlock, total - done );
        // Which sources each group of eight lanes needs; the same decision for every vector width.
        bool tone[2] = {}, modal[2] = {}, metal[2] = {}, noisy[2] = {};
        for ( int l = 0; l < PercLanes; ++l )
        {
            const int g = l / 8;
            tone[g] = tone[g] || m_c.m_wTone[l] != 0.0f;
            modal[g] = modal[g] || m_c.m_wModal[l] != 0.0f;
            metal[g] = metal[g] || m_c.m_wMetal[l] != 0.0f;
            noisy[g] = noisy[g] || m_c.m_wNoise[l] != 0.0f;
        }
        // Scalar preparation: noise, burst restarts, noise decay per sample.
        for ( int l = 0; l < PercLaneSlots; ++l )
        {
            const bool live = l < PercLanes;
            for ( int i = 0; i < n; ++i )
            {
                const size_t row = static_cast<size_t>( i * PercLaneSlots + l );
                m_reset[row] = 0.0f;
                if ( !live )
                {
                    m_noise[row] = 0.0f;
                    m_dNoise[row] = 1.0f;
                    continue;
                }
                m_noise[row] = noisy[l / 8] ? m_noiseRng[l].bipolar() : 0.0f;
                if ( m_burstsLeft[l] > 0 )
                {
                    m_burstTimer[l] -= 1.0;
                    if ( m_burstTimer[l] <= 0.0 )
                    {
                        m_reset[row] = m_burstVel[l];
                        --m_burstsLeft[l];
                        m_burstTimer[l] += m_burstSpacing[l];
                    }
                }
                m_dNoise[row] = m_burstsLeft[l] > 0 ? m_noiseFast[l] : m_noiseTail[l];
            }
        }
        for ( int l = 0; l < PercLaneSlots; l += width )
        {
            const int g = std::min( l / 8, 1 );
            percKernel<VT>( m_s, m_c, l, n, m_noise.data(), m_reset.data(), m_dNoise.data(), tone[g], modal[g],
                            metal[g], m_panning, m_outL.data(), m_outR.data() );
        }
        for ( int i = 0; i < n; ++i )
        {
            float sl = 0.0f, sr = 0.0f;
            for ( int l = 0; l < PercLanes; ++l )
            {
                sl += m_outL[static_cast<size_t>( i * PercLaneSlots + l )];
                sr += m_outR[static_cast<size_t>( i * PercLaneSlots + l )];
            }
            L[done + i] = sl;
            R[done + i] = sr;
        }
        done += n;
    }
}

template void PercKit::processWith<float>( float *, float *, int );
#if PHOS_VEC_PATH != 0
template void PercKit::processWith<VecF>( float *, float *, int );
#endif

void PercKit::process( float *L, float *R, int n )
{
    processWith<VecF>( L, R, n );
}

}   // namespace phos
