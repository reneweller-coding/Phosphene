/**
 * @file Resonance.cpp
 * @brief The master's resonance suppressor (see Resonance.h).
 * @note Copied from Totality `Core/src/fx/Resonance.cpp` (20.10.2026); namespace phos.
 * @author Rene Weller
 */
#include "phos/Resonance.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phos
{

namespace
{
constexpr double Pi = 3.14159265358979323846;   ///< pi
constexpr float  LowHz = 200.0f;      ///< nothing under this is looked at (a bass's harmonics are peaks by nature)
constexpr float  HighHz = 14000.0f;   ///< nor over this
constexpr float  FloorDb = -40.0f;    ///< a bin this quiet (about -95 dBFS) is not a resonance

/** @brief A peak the analysis found: where, how wide, how deep the cut, and how much it matters. */
struct Pick
{
    float m_hz;      ///< where
    float m_q;       ///< how wide (its Q)
    float m_cut;     ///< the cut it asks for, dB (negative)
    float m_score;   ///< how much it matters: its excess, weighted by where it lies
};

}   // namespace

void ResonanceSuppressor::transform( std::vector<float> &re, std::vector<float> &im )
{
    const int n = static_cast<int>( re.size() );
    for ( int i = 1, j = 0; i < n; ++i )
    {
        int bit = n >> 1;
        for ( ; j & bit; bit >>= 1 ) j ^= bit;
        j ^= bit;
        if ( i < j )
        {
            std::swap( re[static_cast<size_t>( i )], re[static_cast<size_t>( j )] );
            std::swap( im[static_cast<size_t>( i )], im[static_cast<size_t>( j )] );
        }
    }
    for ( int len = 2; len <= n; len <<= 1 )
    {
        const double a = -2.0 * Pi / len;
        const double wr = std::cos( a ), wi = std::sin( a );
        for ( int i = 0; i < n; i += len )
        {
            double cr = 1.0, ci = 0.0;
            for ( int j = 0; j < len / 2; ++j )
            {
                const size_t u = static_cast<size_t>( i + j ), v = static_cast<size_t>( i + j + len / 2 );
                const float  tr = static_cast<float>( re[v] * cr - im[v] * ci );
                const float  ti = static_cast<float>( re[v] * ci + im[v] * cr );
                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] += tr;
                im[u] += ti;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

void ResonanceSuppressor::prepare( double sampleRate )
{
    m_rate = sampleRate;
    m_fft = sampleRate <= 64000.0 ? 2048 : ( sampleRate <= 128000.0 ? 4096 : 8192 );   // about 23 Hz a bin
    m_hop = m_fft / 8;
    const int bins = m_fft / 2 + 1;
    m_history.assign( static_cast<size_t>( m_fft ), 0.0f );
    m_window.resize( static_cast<size_t>( m_fft ) );
    for ( int i = 0; i < m_fft; ++i )
        m_window[static_cast<size_t>( i )] = static_cast<float>( 0.5 - 0.5 * std::cos( 2.0 * Pi * i / m_fft ) );
    m_re.assign( static_cast<size_t>( m_fft ), 0.0f );
    m_im.assign( static_cast<size_t>( m_fft ), 0.0f );
    m_smooth.assign( static_cast<size_t>( bins ), -120.0f );
    m_prefix.assign( static_cast<size_t>( bins + 1 ), 0.0f );
    m_lo.assign( static_cast<size_t>( bins ), 0 );
    m_hi.assign( static_cast<size_t>( bins ), 0 );
    m_weight.assign( static_cast<size_t>( bins ), 0.0f );
    const float top = std::min( HighHz, static_cast<float>( 0.45 * sampleRate ) );
    for ( int k = 0; k < bins; ++k )
    {
        // The envelope's span: a third of an octave either way, at least six bins.
        const int lo = std::min( static_cast<int>( std::floor( k * 0.7937005f ) ), k - 6 );
        const int hi = std::max( static_cast<int>( std::ceil( k * 1.2599210f ) ), k + 6 );
        m_lo[static_cast<size_t>( k )] = std::max( 1, lo );
        m_hi[static_cast<size_t>( k )] = std::min( bins - 1, hi );
        // How hard: fully from 1.5 to 8 kHz, 40 % at 200 Hz and at 14 kHz, along log f.
        const float hz = static_cast<float>( k * sampleRate / m_fft );
        float       w = 0.0f;
        if ( hz >= LowHz && hz <= top )
        {
            if ( hz < 1500.0f )
                w = 0.4f + 0.6f * std::log2( hz / LowHz ) / std::log2( 1500.0f / LowHz );
            else if ( hz <= 8000.0f )
                w = 1.0f;
            else
                w = 1.0f - 0.6f * std::log2( hz / 8000.0f ) / std::log2( HighHz / 8000.0f );
        }
        m_weight[static_cast<size_t>( k )] = std::max( 0.0f, w );
    }
    const auto per = [&]( double seconds )
    { return static_cast<float>( 1.0 - std::exp( -m_hop / ( seconds * sampleRate ) ) ); };
    m_attack = per( 0.030 );
    m_release = per( 0.250 );
    m_cutAttack = per( 0.020 );
    m_cutRelease = per( 0.150 );
    reset();
}

void ResonanceSuppressor::reset()
{
    std::fill( m_history.begin(), m_history.end(), 0.0f );
    std::fill( m_smooth.begin(), m_smooth.end(), -120.0f );
    for ( Bell &b : m_bells )
    {
        b = Bell{};
        coefficients( b, b.m_g1, b.m_k1, b.m_m1 );
        b.m_m1 = 0.0f;
        b.m_g0 = b.m_g1;
        b.m_k0 = b.m_k1;
        b.m_m0 = 0.0f;
    }
    m_until = m_hop;
    m_step = 0;
    m_write = 0;
    m_active = m_amount > 0.0f;
}

void ResonanceSuppressor::set( float amount )
{
    m_amount = std::clamp( amount, 0.0f, 1.0f );
    if ( m_amount > 0.0f ) m_active = true;
}

float ResonanceSuppressor::reduction() const
{
    float least = 0.0f;
    for ( const Bell &b : m_bells ) least = std::min( least, b.m_db );
    return least;
}

void ResonanceSuppressor::coefficients( const Bell &b, float &g, float &k, float &m ) const
{
    // Simper's bell: A = 10^(dB/40), g = tan(pi f / fs), k = 1 / (Q A), and the band's output added with k (A^2 - 1).
    const float A = std::pow( 10.0f, b.m_db / 40.0f );
    const float hz = std::min( b.m_hz, static_cast<float>( 0.45 * m_rate ) );
    g = static_cast<float>( std::tan( Pi * hz / m_rate ) );
    k = 1.0f / ( b.m_q * A );
    m = k * ( A * A - 1.0f );
}

void ResonanceSuppressor::analyse()
{
    const int    n = m_fft, bins = n / 2 + 1, mask = n - 1;
    const float  amount = m_amount;
    const double binHz = m_rate / n;
    for ( int j = 0; j < n; ++j )
    {
        m_re[static_cast<size_t>( j )] =
            m_history[static_cast<size_t>( ( m_write + j ) & mask )] * m_window[static_cast<size_t>( j )];
        m_im[static_cast<size_t>( j )] = 0.0f;
    }
    transform( m_re, m_im );
    m_prefix[0] = 0.0f;
    for ( int k = 0; k < bins; ++k )
    {
        const size_t i = static_cast<size_t>( k );
        const float  s = 10.0f * std::log10( m_re[i] * m_re[i] + m_im[i] * m_im[i] + 1e-20f );
        float       &sm = m_smooth[i];
        sm += ( s - sm ) * ( s > sm ? m_attack : m_release );
        m_prefix[i + 1] = m_prefix[i] + sm;
    }

    // The candidates: local peaks over the threshold above the envelope, the largest first, a quarter octave apart.
    std::array<Pick, Bells> picks{};
    int                     count = 0;
    if ( amount > 0.0f )
    {
        const float threshold = 12.0f - 6.0f * amount, share = 0.2f + 0.6f * amount, most = 6.0f * amount;
        for ( int k = 1; k < bins - 1; ++k )
        {
            const size_t i = static_cast<size_t>( k );
            const float  w = m_weight[i], s = m_smooth[i];
            if ( w <= 0.0f || s < FloorDb || s < m_smooth[i - 1] || s <= m_smooth[i + 1] ) continue;
            const int   lo = m_lo[i], hi = m_hi[i];
            const float env = ( m_prefix[static_cast<size_t>( hi + 1 )] - m_prefix[static_cast<size_t>( lo )] ) /
                              static_cast<float>( hi - lo + 1 );
            const float over = s - env - threshold;
            if ( over <= 0.0f ) continue;
            const float score = over * w;
            // Where the peak lies between the bins (a parabola through it and its neighbours), how wide it is.
            const float a = m_smooth[i - 1], c = m_smooth[i + 1], den = a - 2.0f * s + c;
            const float d = den < 0.0f ? std::clamp( 0.5f * ( a - c ) / den, -0.5f, 0.5f ) : 0.0f;
            const float hz = static_cast<float>( ( k + d ) * binHz );
            int         l = k, r = k;
            while ( l > 1 && m_smooth[static_cast<size_t>( l - 1 )] > s - 3.0f && k - l < 64 ) --l;
            while ( r < bins - 2 && m_smooth[static_cast<size_t>( r + 1 )] > s - 3.0f && r - k < 64 ) ++r;
            const float width = static_cast<float>( std::max( 1.5, static_cast<double>( r - l + 1 ) ) * binHz );
            const Pick  p{ hz, std::clamp( hz / width, 1.5f, 12.0f ), -std::min( most, share * over * w ), score };
            // Keep the largest Bells, a quarter octave apart: a nearer, larger one wins.
            bool placed = false;
            for ( int q = 0; q < count && !placed; ++q )
                if ( std::fabs( std::log2( picks[static_cast<size_t>( q )].m_hz / hz ) ) < 0.25f )
                {
                    if ( score > picks[static_cast<size_t>( q )].m_score ) picks[static_cast<size_t>( q )] = p;
                    placed = true;
                }
            if ( placed ) continue;
            if ( count < Bells )
                picks[static_cast<size_t>( count++ )] = p;
            else
            {
                int least = 0;
                for ( int q = 1; q < count; ++q )
                    if ( picks[static_cast<size_t>( q )].m_score < picks[static_cast<size_t>( least )].m_score )
                        least = q;
                if ( score > picks[static_cast<size_t>( least )].m_score ) picks[static_cast<size_t>( least )] = p;
            }
        }
        std::sort( picks.begin(), picks.begin() + count,
                   []( const Pick &x, const Pick &y ) { return x.m_score > y.m_score; } );
    }

    // The bells: each pick to the bell that already follows it (a sixth of an octave), else to a free bell.
    bool taken[Bells] = {};
    for ( Bell &b : m_bells ) b.m_aimDb = 0.0f;
    for ( int p = 0; p < count; ++p )
    {
        const Pick &pk = picks[static_cast<size_t>( p )];
        int         best = -1;
        float       nearest = 1.0f / 6.0f;
        for ( int j = 0; j < Bells; ++j )
        {
            const Bell &b = m_bells[static_cast<size_t>( j )];
            const float dist = std::fabs( std::log2( b.m_hz / pk.m_hz ) );
            if ( !taken[j] && b.m_db < 0.0f && dist < nearest )
            {
                nearest = dist;
                best = j;
            }
        }
        if ( best < 0 )
            for ( int j = 0; j < Bells && best < 0; ++j )
                if ( !taken[j] && m_bells[static_cast<size_t>( j )].m_db >= 0.0f ) best = j;
        if ( best < 0 ) continue;
        Bell &b = m_bells[static_cast<size_t>( best )];
        taken[best] = true;
        if ( b.m_db >= 0.0f )
        {   // a free bell jumps to the peak: at 0 dB its frequency is not heard
            b.m_hz = pk.m_hz;
            b.m_q = pk.m_q;
        }
        b.m_aimHz = pk.m_hz;
        b.m_aimQ = pk.m_q;
        b.m_aimDb = pk.m_cut;
    }
    bool any = false;
    for ( Bell &b : m_bells )
    {
        b.m_db += ( b.m_aimDb - b.m_db ) * ( b.m_aimDb < b.m_db ? m_cutAttack : m_cutRelease );
        if ( b.m_aimDb == 0.0f && b.m_db > -0.01f ) b.m_db = 0.0f;
        if ( b.m_db < 0.0f )
        {
            b.m_hz *= std::sqrt( b.m_aimHz / b.m_hz );   // halfway along log f each analysis
            b.m_q += 0.5f * ( b.m_aimQ - b.m_q );
        }
        const bool wasNeutral = b.m_m1 == 0.0f;
        b.m_g0 = b.m_g1;
        b.m_k0 = b.m_k1;
        b.m_m0 = b.m_m1;
        if ( b.m_db < 0.0f )
        {
            coefficients( b, b.m_g1, b.m_k1, b.m_m1 );
            if ( wasNeutral )
            {   // a bell coming in starts from rest where it is, its gain from 0 dB
                b.m_g0 = b.m_g1;
                b.m_k0 = b.m_k1;
                b.m_ic1[0] = b.m_ic1[1] = b.m_ic2[0] = b.m_ic2[1] = 0.0f;
            }
            any = true;
        }
        else
            b.m_m1 = 0.0f;
        any = any || b.m_m0 != 0.0f;
    }
    m_active = any || amount > 0.0f;
}

void ResonanceSuppressor::process( float *L, float *R, int n )
{
    const int mask = m_fft - 1;
    if ( !m_active )
    {   // off and every bell back at 0 dB: the history alone, so that it is current when it is turned up
        for ( int i = 0; i < n; ++i )
        {
            m_history[static_cast<size_t>( m_write )] = 0.5f * ( L[i] + R[i] );
            m_write = ( m_write + 1 ) & mask;
        }
        m_until = ( ( m_until - 1 - n ) % m_hop + m_hop ) % m_hop + 1;   // the analyses keep their places
        return;
    }
    const float inv = 1.0f / static_cast<float>( m_hop );
    for ( int i = 0; i < n; ++i )
    {
        m_history[static_cast<size_t>( m_write )] = 0.5f * ( L[i] + R[i] );
        m_write = ( m_write + 1 ) & mask;
        if ( --m_until <= 0 )
        {
            analyse();
            m_until = m_hop;
            m_step = 0;
        }
        const float t = static_cast<float>( ++m_step ) * inv;
        float       x[2] = { L[i], R[i] };
        for ( Bell &b : m_bells )
        {
            if ( b.m_m0 == 0.0f && b.m_m1 == 0.0f ) continue;
            const float g = b.m_g0 + ( b.m_g1 - b.m_g0 ) * t;
            const float k = b.m_k0 + ( b.m_k1 - b.m_k0 ) * t;
            const float m = b.m_m0 + ( b.m_m1 - b.m_m0 ) * t;
            const float a1 = 1.0f / ( 1.0f + g * ( g + k ) ), a2 = g * a1, a3 = g * a2;
            for ( int c = 0; c < 2; ++c )
            {
                const float v3 = x[c] - b.m_ic2[c];
                const float v1 = a1 * b.m_ic1[c] + a2 * v3;
                const float v2 = b.m_ic2[c] + a2 * b.m_ic1[c] + a3 * v3;
                b.m_ic1[c] = 2.0f * v1 - b.m_ic1[c];
                b.m_ic2[c] = 2.0f * v2 - b.m_ic2[c];
                x[c] += m * v1;
            }
        }
        L[i] = x[0];
        R[i] = x[1];
    }
}

}   // namespace phos
