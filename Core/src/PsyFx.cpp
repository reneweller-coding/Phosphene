/**
 * @file PsyFx.cpp
 * @brief Flanger, phaser, frequency shifter, their chain, and the stutter (PsyFx.h).
 * @author Rene Weller
 */
#include "phos/PsyFx.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos
{

namespace
{
constexpr double PiD = 3.141592653589793;   ///< pi

/**
 * @brief Raised-cosine ramp 0..1 for u in [0, 1], clamped outside.
 * @param[in] u  the place, 0..1
 * @return the ramp, 0..1
 */
inline float smoothRamp( double u )
{
    if ( u >= 1.0 ) return 1.0f;
    if ( u <= 0.0 ) return 0.0f;
    return static_cast<float>( 0.5 - 0.5 * std::cos( PiD * u ) );
}

/**
 * @brief The fractional part of a beat position divided by a period: an LFO phase in [0, 1).
 * @param[in] beat    the beat
 * @param[in] period  the period in beats
 * @return the phase, 0..1
 */
inline double lfoPhase( double beat, double period )
{
    const double p = beat / std::max( 1e-6, period );
    return p - std::floor( p );
}

/**
 * @brief The two all-pass chains of the 90-degree network (PsyFx.h), four second-order sections each.
 *
 * Their squares are what the difference equation uses. The one-sample delay belongs to chain A. The
 * phase difference was measured, not taken on trust: the self test drives the pair with sines from
 * 50 Hz to 20 kHz and reads the angle between the two outputs.
 */
constexpr double HilbertA[4] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
constexpr double HilbertB[4] = { 0.4021921162426, 0.8561710882420, 0.9722909545651,
                                 0.9952884791278 };   ///< the second chain's all-pass coefficients
}   // namespace

// ---------------------------------------------------------------------------------------------
// Flanger

void Flanger::prepare( double sampleRate )
{
    m_sr = sampleRate;
    size_t n = 1;
    while ( static_cast<double>( n ) < 0.02 * sampleRate ) n <<= 1;   // 20 ms: well past the 6.3 ms sweep
    m_bufL.assign( n, 0.0f );
    m_bufR.assign( n, 0.0f );
    m_mask = n - 1;
    reset();
}

void Flanger::reset()
{
    std::fill( m_bufL.begin(), m_bufL.end(), 0.0f );
    std::fill( m_bufR.begin(), m_bufR.end(), 0.0f );
    m_write = 0;
}

void Flanger::set( float periodBeats, float depth, float feedback, float mix )
{
    m_period = std::max( 0.0625f, periodBeats );
    m_depth = clampv( depth, 0.0f, 1.0f );
    m_fb = clampv( feedback, -0.9f, 0.9f );
    m_mix = clampv( mix, 0.0f, 1.0f );
}

float Flanger::read( const std::vector<float> &b, double delay ) const
{
    const double pos = static_cast<double>( m_write ) - delay;
    const double fl = std::floor( pos );
    const float  frac = static_cast<float>( pos - fl );
    const size_t i0 = static_cast<size_t>( static_cast<long long>( fl ) ) & m_mask;
    const size_t i1 = ( i0 + 1 ) & m_mask;
    return b[i0] + frac * ( b[i1] - b[i0] );
}

void Flanger::tick( float &l, float &r, double beat )
{
    const double ph = lfoPhase( beat, m_period );
    // 0.3 ms to 6 ms: the whole audible comb range, from a metallic sheen to a jet sweep.
    const double spanMs = 5.7 * m_depth;
    const double dl = ( 0.3 + spanMs * ( 0.5 - 0.5 * std::cos( 2.0 * PiD * ph ) ) ) * 0.001 * m_sr;
    const double dr = ( 0.3 + spanMs * ( 0.5 - 0.5 * std::cos( 2.0 * PiD * ( ph + 0.25 ) ) ) ) * 0.001 * m_sr;
    const float  yl = read( m_bufL, dl ), yr = read( m_bufR, dr );
    m_bufL[m_write] = l + m_fb * yl;
    m_bufR[m_write] = r + m_fb * yr;
    m_write = ( m_write + 1 ) & m_mask;
    l = ( 1.0f - m_mix ) * l + m_mix * 0.5f * ( l + yl );
    r = ( 1.0f - m_mix ) * r + m_mix * 0.5f * ( r + yr );
}

// ---------------------------------------------------------------------------------------------
// Phaser

void Phaser::prepare( double sampleRate )
{
    m_sr = sampleRate;
    reset();
}

void Phaser::reset()
{
    for ( int k = 0; k < Stages; ++k ) m_zL[k] = m_zR[k] = 0.0f;
    m_fbL = m_fbR = 0.0f;
}

void Phaser::set( float periodBeats, float depth, float feedback, float mix )
{
    m_period = std::max( 0.0625f, periodBeats );
    m_depth = clampv( depth, 0.0f, 1.0f );
    m_fb = clampv( feedback, 0.0f, 0.9f );
    m_mix = clampv( mix, 0.0f, 1.0f );
}

float Phaser::stage( float x, float a, float *z, float &fbState )
{
    // Six first-order all-passes H = (a + z^-1) / (1 + a z^-1) in cascade, transposed direct form II,
    // with the last stage fed back to the input.
    float y = x + m_fb * fbState;
    for ( int k = 0; k < Stages; ++k )
    {
        const float o = a * y + z[k];
        z[k] = y - a * o;
        y = o;
    }
    fbState = y;
    return y;
}

void Phaser::tick( float &l, float &r, double beat )
{
    const double ph = lfoPhase( beat, m_period );
    auto         coef = [&]( double phase )
    {
        const double lfo = 0.5 - 0.5 * std::cos( 2.0 * PiD * phase );
        const double f = std::min( 200.0 * std::pow( 2.0, 5.0 * m_depth * lfo ), 0.45 * m_sr );
        const double t = std::tan( PiD * f / m_sr );
        return static_cast<float>( ( t - 1.0 ) / ( t + 1.0 ) );
    };
    const float yl = stage( l, coef( ph ), m_zL, m_fbL );
    const float yr = stage( r, coef( ph + 0.25 ), m_zR, m_fbR );
    l = ( 1.0f - m_mix ) * l + m_mix * 0.5f * ( l + yl );
    r = ( 1.0f - m_mix ) * r + m_mix * 0.5f * ( r + yr );
}

// ---------------------------------------------------------------------------------------------
// Frequency shifter

void HilbertPair::reset()
{
    for ( int k = 0; k < 4; ++k )
        for ( int j = 0; j < 2; ++j ) m_xa[k][j] = m_ya[k][j] = m_xb[k][j] = m_yb[k][j] = 0.0;
    m_delayA = 0.0;
}

void HilbertPair::tick( double x, double &i, double &q )
{
    double a = x, b = x;
    for ( int k = 0; k < 4; ++k )
    {
        const double ca = HilbertA[k] * HilbertA[k];
        const double ya0 = ca * ( a + m_ya[k][1] ) - m_xa[k][1];
        m_xa[k][1] = m_xa[k][0];
        m_xa[k][0] = a;
        m_ya[k][1] = m_ya[k][0];
        m_ya[k][0] = ya0;
        a = ya0;
        const double cb = HilbertB[k] * HilbertB[k];
        const double yb0 = cb * ( b + m_yb[k][1] ) - m_xb[k][1];
        m_xb[k][1] = m_xb[k][0];
        m_xb[k][0] = b;
        m_yb[k][1] = m_yb[k][0];
        m_yb[k][0] = yb0;
        b = yb0;
    }
    // Chain A lags chain B by 90 degrees once A is delayed by a sample: B is the in-phase output.
    q = m_delayA;
    m_delayA = a;
    i = b;
}

void FreqShifter::prepare( double sampleRate )
{
    m_sr = sampleRate;
    reset();
}

void FreqShifter::reset()
{
    m_hl.reset();
    m_hr.reset();
    m_phase = 0.0;
}

void FreqShifter::set( float hz, float mix )
{
    m_hz = hz;
    m_mix = clampv( mix, 0.0f, 1.0f );
}

void FreqShifter::tick( float &l, float &r )
{
    double il, ql, ir, qr;
    m_hl.tick( l, il, ql );
    m_hr.tick( r, ir, qr );
    m_phase += static_cast<double>( m_hz ) / m_sr;
    m_phase -= std::floor( m_phase );
    const double c = std::cos( 2.0 * PiD * m_phase ), s = std::sin( 2.0 * PiD * m_phase );
    // cos(psi + theta) = I cos(theta) - Q sin(theta): every partial moves up by m_hz. The right channel's
    // carrier runs 90 degrees ahead (cos -> -sin, sin -> cos), which decorrelates the two sides without
    // putting them in antiphase.
    const float sl = static_cast<float>( il * c - ql * s );
    const float sr = static_cast<float>( ir * ( -s ) - qr * c );
    // At zero shift the wet path is only the all-pass network's phase, which beside the dry signal
    // would comb; the wet share therefore fades in over the first 5 Hz of shift.
    const float m = m_mix * std::min( 1.0f, std::fabs( m_hz ) / 5.0f );
    l = ( 1.0f - m ) * l + m * sl;
    r = ( 1.0f - m ) * r + m * sr;
}

// ---------------------------------------------------------------------------------------------
// Chain

void PsyFxChain::prepare( double sampleRate )
{
    m_flanger.prepare( sampleRate );
    m_phaser.prepare( sampleRate );
    m_shifter.prepare( sampleRate );
}

void PsyFxChain::reset()
{
    m_flanger.reset();
    m_phaser.reset();
    m_shifter.reset();
    m_motionHz = m_motionFlange = 0.0f;
}

void PsyFxChain::update( const float *v )
{
    for ( int k = 0; k < psyfx::COUNT; ++k ) m_v[k] = v[k];
    apply();
}

void PsyFxChain::setMotion( float shiftHz, float flangerMix )
{
    m_motionHz = shiftHz;
    m_motionFlange = flangerMix;
    apply();
}

void PsyFxChain::apply()
{
    m_flanger.set( m_v[psyfx::FLANGER_BEATS], m_v[psyfx::FLANGER_DEPTH], m_v[psyfx::FLANGER_FEEDBACK],
                   m_v[psyfx::FLANGER_MIX] + m_motionFlange );
    m_phaser.set( m_v[psyfx::PHASER_BEATS], m_v[psyfx::PHASER_DEPTH], m_v[psyfx::PHASER_FEEDBACK],
                  m_v[psyfx::PHASER_MIX] );
    m_shifter.set( m_v[psyfx::SHIFT_HZ] + m_motionHz, m_v[psyfx::SHIFT_MIX] );
}

void PsyFxChain::process( float *L, float *R, int n, double beat0, double bps )
{
    for ( int i = 0; i < n; ++i )
    {
        const double beat = beat0 + static_cast<double>( i ) * bps;
        float        l = L[i], r = R[i];
        m_flanger.tick( l, r, beat );
        m_phaser.tick( l, r, beat );
        m_shifter.tick( l, r );
        L[i] = l;
        R[i] = r;
    }
}

// ---------------------------------------------------------------------------------------------
// Stutter

void Stutter::prepare( double sampleRate )
{
    m_recL.assign( static_cast<size_t>( 2.0 * sampleRate ), 0.0f );
    m_recR.assign( m_recL.size(), 0.0f );
    m_fade = std::max( 8, static_cast<int>( 0.001 * sampleRate ) );
    reset();
}

void Stutter::reset()
{
    m_on = false;
    m_pos = m_length = 0;
    m_slice = 1;
}

void Stutter::trigger( int lengthSamples, int sliceSamples )
{
    m_on = lengthSamples > 0;
    m_pos = 0;
    m_length = lengthSamples;
    m_slice = std::clamp<long long>( sliceSamples, 4LL * m_fade, static_cast<long long>( m_recL.size() ) );
}

void Stutter::tick( float &l, float &r )
{
    if ( !m_on ) return;
    const size_t cap = m_recL.size();
    if ( static_cast<size_t>( m_pos ) < cap )
    {
        m_recL[static_cast<size_t>( m_pos )] = l;
        m_recR[static_cast<size_t>( m_pos )] = r;
    }
    // The slice, halved over the last quarter of the event (a roll), and where in it this sample is.
    const long long q0 = m_length - m_length / 4;
    const long long sl = m_pos < q0 ? m_slice : std::max<long long>( m_slice / 2, 2LL * m_fade );
    const long long j = m_pos < q0 ? m_pos % sl : ( m_pos - q0 ) % sl;
    const bool      first = m_pos < m_slice;   // the first slice is the live signal itself
    const double    f = static_cast<double>( m_fade );
    const float     head = first ? 1.0f : smoothRamp( static_cast<double>( j ) / f );
    const float     tail = smoothRamp( static_cast<double>( sl - 1 - j ) / f );
    const size_t    at = static_cast<size_t>( std::min<long long>( j, static_cast<long long>( cap ) - 1 ) );
    const float     g = std::min( head, tail );
    const float     rl = m_recL[at] * g, rr = m_recR[at] * g;
    // Back to the live signal over the last millisecond of the event.
    const float e = smoothRamp( static_cast<double>( m_length - m_pos ) / f );
    l = e * rl + ( 1.0f - e ) * l;
    r = e * rr + ( 1.0f - e ) * r;
    if ( ++m_pos >= m_length ) m_on = false;
}

}   // namespace phos
