/**
 * @file Image.cpp
 * @brief The master's image (see Image.h).
 * @note Copied from Totality `Core/src/fx/Image.cpp` (20.10.2026); namespace phos.
 * @author Rene Weller
 */
#include "phos/Image.h"

#include <algorithm>
#include <cmath>

namespace phos
{

namespace
{
constexpr double Pi = 3.14159265358979323846;   ///< pi
/** @brief The bands' edges, Hz: the lower (0: none) and the upper (0: none). */
constexpr float Edges[ImageMatch::Bands][2] = { { 0.0f, 180.0f },
                                                { 180.0f, 500.0f },
                                                { 500.0f, 1400.0f },
                                                { 1400.0f, 5600.0f },
                                                { 5600.0f, 0.0f } };
/** @brief The side's shelf and bells, one per band. */
const ToneBand SideBands[ImageMatch::Bands] = { { ToneBandE::LOW_SHELF, 180.0f, 0.7071f },
                                                { ToneBandE::BELL, 300.0f, 1.0f },
                                                { ToneBandE::BELL, 840.0f, 0.95f },
                                                { ToneBandE::BELL, 2800.0f, 0.67f },
                                                { ToneBandE::HIGH_SHELF, 5600.0f, 0.7071f } };
constexpr float GuardDb = -0.5f;     ///< no band's side over this against its mid
constexpr float UpDb = 4.0f;         ///< the most a side is raised
constexpr float DownDb = -12.0f;     ///< the most it is lowered
constexpr float FloorDb = -40.0f;    ///< a side further under its mid is not raised (there is nothing to raise)
constexpr float GateDb = 8.0f;       ///< blocks within this of the loudest count
}   // namespace

float ImageMatch::run( Meter &m, float x )
{
    // Two high passes and two low passes, each Simper's trapezoidal SVF with Butterworth damping.
    for ( int s = 0; s < 4; ++s )
    {
        if ( ( s < 2 && !m.m_high ) || ( s >= 2 && !m.m_low ) ) continue;
        const float a1 = 1.0f / ( 1.0f + m.m_g[s] * ( m.m_g[s] + m.m_k ) ), a2 = m.m_g[s] * a1, a3 = m.m_g[s] * a2;
        const float v3 = x - m.m_s2[s];
        const float v1 = a1 * m.m_s1[s] + a2 * v3;
        const float v2 = m.m_s2[s] + a2 * m.m_s1[s] + a3 * v3;
        m.m_s1[s] = 2.0f * v1 - m.m_s1[s];
        m.m_s2[s] = 2.0f * v2 - m.m_s2[s];
        x = s < 2 ? x - m.m_k * v1 - v2 : v2;
    }
    return x;
}

void ImageMatch::prepare( double sampleRate )
{
    m_rate = sampleRate;
    for ( int b = 0; b < Bands; ++b )
        for ( std::array<Meter, Bands> *set : { &m_mid, &m_side } )
        {
            Meter &m = ( *set )[static_cast<size_t>( b )];
            m = Meter{};
            m.m_high = Edges[b][0] > 0.0f;
            m.m_low = Edges[b][1] > 0.0f;
            for ( int s = 0; s < 4; ++s )
            {
                const float hz = s < 2 ? Edges[b][0] : Edges[b][1];
                const double f = std::min( static_cast<double>( hz ), 0.45 * sampleRate );
                m.m_g[s] = hz > 0.0f ? static_cast<float>( std::tan( Pi * f / sampleRate ) ) : 0.0f;
            }
        }
    m_blockLength = std::max( 1, static_cast<int>( 0.1 * sampleRate ) );
    m_avgCoef = static_cast<float>( 1.0 - std::exp( -0.1 / 8.0 ) );
    m_glide = 0.05f;
    m_eq.prepare( sampleRate, SideBands, Bands );
    reset();
}

void ImageMatch::reset()
{
    for ( std::array<Meter, Bands> *set : { &m_mid, &m_side } )
        for ( Meter &m : *set )
            for ( int s = 0; s < 4; ++s ) m.m_s1[s] = m.m_s2[s] = 0.0f;
    m_blockMid.fill( 0.0 );
    m_blockSide.fill( 0.0 );
    m_avgMid.fill( 0.0 );
    m_avgSide.fill( 0.0 );
    m_trim.fill( 0.0f );
    m_loudest = 0.0;
    m_count = 0;
    m_heard = false;
    m_eq.setGains( m_trim.data() );
    m_state.reset();
}

void ImageMatch::setRanges( const float *lo, const float *hi )
{
    for ( int b = 0; b < Bands; ++b )
    {
        m_lo[static_cast<size_t>( b )] = lo[b];
        m_hi[static_cast<size_t>( b )] = std::min( hi[b], GuardDb );
    }
}

void ImageMatch::set( float amount )
{
    m_amount = std::clamp( amount, 0.0f, 1.0f );
}

float ImageMatch::width( int band ) const
{
    const size_t b = static_cast<size_t>( band );
    if ( !m_heard || m_avgMid[b] <= 1e-20 ) return -120.0f;
    return static_cast<float>( 10.0 * std::log10( std::max( m_avgSide[b], 1e-30 ) / m_avgMid[b] ) );
}

void ImageMatch::block()
{
    // The gate: this block counts if it is within GateDb of the loudest of about the last minute.
    double total = 0.0;
    for ( int b = 0; b < Bands; ++b )
        total += m_blockMid[static_cast<size_t>( b )] + m_blockSide[static_cast<size_t>( b )];
    m_loudest = std::max( m_loudest * std::pow( 10.0, -0.1 / 60.0 ), total );   // falling 1 dB in 6 s
    if ( total > 1e-12 && total >= m_loudest * std::pow( 10.0, -GateDb / 10.0 ) )
    {
        for ( int b = 0; b < Bands; ++b )
        {
            const size_t i = static_cast<size_t>( b );
            if ( !m_heard )
            {
                m_avgMid[i] = m_blockMid[i];
                m_avgSide[i] = m_blockSide[i];
            }
            else
            {
                m_avgMid[i] += ( m_blockMid[i] - m_avgMid[i] ) * m_avgCoef;
                m_avgSide[i] += ( m_blockSide[i] - m_avgSide[i] ) * m_avgCoef;
            }
        }
        m_heard = true;
    }
    m_blockMid.fill( 0.0 );
    m_blockSide.fill( 0.0 );
    // The aims, and the trims a step towards them.
    bool moved = false;
    for ( int b = 0; b < Bands; ++b )
    {
        const size_t i = static_cast<size_t>( b );
        float        aim = 0.0f;
        const float  w = width( b );
        if ( m_amount > 0.0f && w > -100.0f )
        {
            if ( w > m_hi[i] )
                aim = m_hi[i] - w;
            else if ( w < m_lo[i] && w > FloorDb )
                aim = std::min( m_lo[i] - w, UpDb );
            aim *= m_amount;
            aim = std::min( aim, GuardDb - w );   // the guard, whatever the amount
            aim = std::clamp( aim, DownDb, UpDb );
        }
        const float step = std::clamp( aim - m_trim[i], -m_glide, m_glide );
        if ( step != 0.0f )
        {
            m_trim[i] += step;
            moved = true;
        }
    }
    if ( moved ) m_eq.setGains( m_trim.data() );
}

void ImageMatch::process( float *L, float *R, int n )
{
    if ( m_amount <= 0.0f && m_eq.flat() ) return;   // off, and every trim back at 0 dB
    float mid[256], side[256];
    for ( int done = 0; done < n; )
    {
        // Split at every block's end, so the trims change at the same sample whatever the host's block size.
        const int len = std::min( { 256, n - done, m_blockLength - m_count } );
        for ( int i = 0; i < len; ++i )
        {
            const float m = 0.5f * ( L[done + i] + R[done + i] ), s = 0.5f * ( L[done + i] - R[done + i] );
            mid[i] = m;
            side[i] = s;
            for ( int b = 0; b < Bands; ++b )
            {
                const size_t k = static_cast<size_t>( b );
                const float  bm = run( m_mid[k], m ), bs = run( m_side[k], s );
                m_blockMid[k] += static_cast<double>( bm ) * bm;
                m_blockSide[k] += static_cast<double>( bs ) * bs;
            }
        }
        m_eq.process( m_state, side, nullptr, len );
        for ( int i = 0; i < len; ++i )
        {
            L[done + i] = mid[i] + side[i];
            R[done + i] = mid[i] - side[i];
        }
        done += len;
        m_count += len;
        if ( m_count >= m_blockLength )
        {
            m_count = 0;
            block();
        }
    }
}

}   // namespace phos
