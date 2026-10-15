/**
 * @file Loudness.cpp
 * @brief BS.1770 meter implementation.
 * @note Copied from Noctuary `Core/src/Loudness.cpp` at b60a2fe (15.09.2026), sone model removed.
 * @author Rene Weller
 */
#include "phos/Loudness.h"
#include "phos/Dynamics.h"
#include "phos/Dsp.h"
#include <algorithm>
#include <cmath>

namespace phos
{

void KFilter::prepare( double sr )
{
    // Not the RBJ cookbook: the cookbook shelf with the standard's f0, Q and gain lands about two
    // per cent away from the published 48 kHz coefficients. This formulation reproduces them.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan( Pi * f0 / sr );
        const double Vh = std::pow( 10.0, G / 20.0 );
        const double Vb = std::pow( Vh, 0.4996667741545416 );
        const double a0 = 1.0 + K / Q + K * K;
        m_shelf.m_b0 = static_cast<float>( ( Vh + Vb * K / Q + K * K ) / a0 );
        m_shelf.m_b1 = static_cast<float>( 2.0 * ( K * K - Vh ) / a0 );
        m_shelf.m_b2 = static_cast<float>( ( Vh - Vb * K / Q + K * K ) / a0 );
        m_shelf.m_a1 = static_cast<float>( 2.0 * ( K * K - 1.0 ) / a0 );
        m_shelf.m_a2 = static_cast<float>( ( 1.0 - K / Q + K * K ) / a0 );
    }
    {   // The RLB high-pass; its numerator is exactly 1, -2, 1 as the standard prints it.
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan( Pi * f0 / sr );
        const double d = 1.0 + K / Q + K * K;
        m_hp.m_b0 = 1.0f;
        m_hp.m_b1 = -2.0f;
        m_hp.m_b2 = 1.0f;
        m_hp.m_a1 = static_cast<float>( 2.0 * ( K * K - 1.0 ) / d );
        m_hp.m_a2 = static_cast<float>( ( 1.0 - K / Q + K * K ) / d );
    }
    reset();
}

void KFilter::reset()
{
    m_shelf.reset();
    m_hp.reset();
}

float KFilter::process( float x )
{
    return m_hp.process( m_shelf.process( x ) );
}

void LoudnessMeter::prepare( double sampleRate )
{
    m_sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_hopLen = std::max( 1, static_cast<int>( m_sr * 0.1 ) );
    m_blockLen = m_hopLen * HopsPerBlock;
    m_kL.prepare( m_sr );
    m_kR.prepare( m_sr );
    m_sumL.assign( static_cast<size_t>( HopsPerShort ), 0.0 );
    m_sumR.assign( static_cast<size_t>( HopsPerShort ), 0.0 );
    m_blocks.prepare( BlockLog );
    m_shortBlocks.prepare( BlockLog );
    reset();
}

void LoudnessMeter::reset()
{
    m_kL.reset();
    m_kR.reset();
    std::fill( m_sumL.begin(), m_sumL.end(), 0.0 );
    std::fill( m_sumR.begin(), m_sumR.end(), 0.0 );
    m_ringPos = 0;
    m_ringFilled = 0;
    m_hopPos = 0;
    m_hopL = m_hopR = 0.0;
    m_hopSamples = 0;
    m_blocks.clear();
    m_shortBlocks.clear();
    m_truePeak = 0.0;
    m_seconds = 0.0;
    m_lastShort = -120.0f;
    for ( int i = 0; i < 2 * TruePeakInterpolator::History; ++i )
    {
        m_tpHistL[i] = 0.0f;
        m_tpHistR[i] = 0.0f;
    }
    m_tpPos = 0;
}

namespace
{

/**
 * @brief Loudness of a mean-square pair, both channel weights one.
 * @param[in] msL  the left mean square
 * @param[in] msR  the right mean square
 * @return the loudness in LUFS
 */
inline float lufs( double msL, double msR )
{
    const double s = msL + msR;
    return s > 1.0e-12 ? static_cast<float>( -0.691 + 10.0 * std::log10( s ) ) : -120.0f;
}

}   // namespace

void LoudnessMeter::pushBlock()
{
    m_sumL[static_cast<size_t>( m_ringPos )] = m_hopSamples > 0 ? m_hopL / static_cast<double>( m_hopSamples ) : 0.0;
    m_sumR[static_cast<size_t>( m_ringPos )] = m_hopSamples > 0 ? m_hopR / static_cast<double>( m_hopSamples ) : 0.0;
    m_ringPos = ( m_ringPos + 1 ) % HopsPerShort;
    if ( m_ringFilled < HopsPerShort ) ++m_ringFilled;
    m_hopL = m_hopR = 0.0;
    m_hopSamples = 0;

    auto meanOver = [this]( int hops, double &l, double &r )
    {
        const int n = std::min( hops, m_ringFilled );
        l = r = 0.0;
        for ( int k = 1; k <= n; ++k )
        {
            const int idx = ( m_ringPos - k + HopsPerShort ) % HopsPerShort;
            l += m_sumL[static_cast<size_t>( idx )];
            r += m_sumR[static_cast<size_t>( idx )];
        }
        if ( n > 0 )
        {
            l /= n;
            r /= n;
        }
        return n;
    };

    double bl = 0.0, br = 0.0;
    if ( meanOver( HopsPerBlock, bl, br ) == HopsPerBlock ) m_blocks.push( lufs( bl, br ) );
    double sl = 0.0, sr = 0.0;
    if ( meanOver( HopsPerShort, sl, sr ) == HopsPerShort )
    {
        m_lastShort = lufs( sl, sr );
        m_shortBlocks.push( m_lastShort );
    }
}

void LoudnessMeter::process( const float *L, const float *R, int n )
{
    constexpr int T = TruePeakInterpolator::History;
    constexpr int H = TruePeakInterpolator::Half;
    const double  bound = m_tpInterp.gainBound();
    for ( int i = 0; i < n; ++i )
    {
        const float l = L[i], r = R[i];
        // True peak with the 8x Kaiser-sinc interpolator of Dynamics.h, the same estimate the limiter
        // holds its ceiling with (the 4-point Lagrange of Noctuary's meter read up to 0.1 dB higher).
        m_tpHistL[m_tpPos] = l;
        m_tpHistL[m_tpPos + T] = l;
        m_tpHistR[m_tpPos] = r;
        m_tpHistR[m_tpPos + T] = r;
        const float *wl = m_tpHistL + m_tpPos + 1;
        const float *wr = m_tpHistR + m_tpPos + 1;
        m_tpPos = m_tpPos + 1 == T ? 0 : m_tpPos + 1;
        // Only a window whose largest sample could reach the running maximum can raise it: no
        // interpolated point exceeds gainBound() times that sample (TruePeakInterpolator::gainBound).
        // Skipping the rest leaves the reported maximum exactly what the full computation gives.
        m_truePeak = std::max( { m_truePeak, std::fabs( static_cast<double>( wl[H - 1] ) ),
                                 std::fabs( static_cast<double>( wr[H - 1] ) ) } );
        if ( static_cast<double>( TruePeakInterpolator::windowPeak( wl + H - 1 ) ) * bound > m_truePeak )
            m_truePeak = std::max( m_truePeak, m_tpInterp.between( wl + H - 1 ) );
        if ( static_cast<double>( TruePeakInterpolator::windowPeak( wr + H - 1 ) ) * bound > m_truePeak )
            m_truePeak = std::max( m_truePeak, m_tpInterp.between( wr + H - 1 ) );

        const float kl = m_kL.process( l ), kr = m_kR.process( r );
        m_hopL += static_cast<double>( kl ) * kl;
        m_hopR += static_cast<double>( kr ) * kr;
        ++m_hopSamples;
        if ( ++m_hopPos >= m_hopLen )
        {
            m_hopPos = 0;
            pushBlock();
        }
    }
    m_seconds += static_cast<double>( n ) / m_sr;
}

LoudnessReading LoudnessMeter::read() const
{
    LoudnessReading out;
    out.m_seconds = static_cast<float>( m_seconds );
    out.m_shortTerm = m_lastShort;
    {
        double    l = 0.0, r = 0.0;
        const int n = std::min( HopsPerBlock, m_ringFilled );
        for ( int k = 1; k <= n; ++k )
        {
            const int idx = ( m_ringPos - k + HopsPerShort ) % HopsPerShort;
            l += m_sumL[static_cast<size_t>( idx )];
            r += m_sumR[static_cast<size_t>( idx )];
        }
        if ( n > 0 ) out.m_momentary = lufs( l / n, r / n );
    }
    // Integrated: absolute gate at -70 LUFS, then a relative gate 10 LU under the mean of the rest.
    if ( !m_blocks.empty() )
    {
        const std::vector<float> bl = m_blocks.snapshot();
        double                   sum = 0.0;
        int                      count = 0;
        for ( float b : bl )
            if ( b > -70.0f )
            {
                sum += std::pow( 10.0, ( b + 0.691 ) / 10.0 );
                ++count;
            }
        if ( count > 0 )
        {
            const float gate = static_cast<float>( -0.691 + 10.0 * std::log10( sum / count ) ) - 10.0f;
            double      s2 = 0.0;
            int         c2 = 0;
            for ( float b : bl )
                if ( b > -70.0f && b > gate )
                {
                    s2 += std::pow( 10.0, ( b + 0.691 ) / 10.0 );
                    ++c2;
                }
            if ( c2 > 0 ) out.m_integrated = static_cast<float>( -0.691 + 10.0 * std::log10( s2 / c2 ) );
        }
    }
    // Loudness range (EBU Tech 3342): short-term values above a -20 LU relative gate, 10th to 95th centile.
    if ( m_shortBlocks.size() >= 10 )
    {
        const std::vector<float> sb = m_shortBlocks.snapshot();
        std::vector<float>       v;
        v.reserve( sb.size() );
        double sum = 0.0;
        int    count = 0;
        for ( float b : sb )
            if ( b > -70.0f )
            {
                sum += std::pow( 10.0, ( b + 0.691 ) / 10.0 );
                ++count;
            }
        if ( count > 0 )
        {
            const float gate = static_cast<float>( -0.691 + 10.0 * std::log10( sum / count ) ) - 20.0f;
            for ( float b : sb )
                if ( b > gate ) v.push_back( b );
            if ( v.size() >= 10 )
            {
                std::sort( v.begin(), v.end() );
                const size_t lo = static_cast<size_t>( 0.10 * static_cast<double>( v.size() - 1 ) );
                const size_t hi = static_cast<size_t>( 0.95 * static_cast<double>( v.size() - 1 ) );
                out.m_range = v[hi] - v[lo];
            }
        }
    }
    out.m_truePeak = m_truePeak > 1.0e-9 ? static_cast<float>( 20.0 * std::log10( m_truePeak ) ) : -120.0f;
    if ( out.m_shortTerm > -119.0f && out.m_truePeak > -119.0f ) out.m_crest = out.m_truePeak - out.m_shortTerm;
    return out;
}

}   // namespace phos
