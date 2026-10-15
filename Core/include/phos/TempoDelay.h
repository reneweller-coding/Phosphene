/**
 * @file TempoDelay.h
 * @brief Tempo-synchronised stereo feedback delay with a band limit inside the loop.
 *
 * Two delay lines, left and right, each with its own time in beats (a dotted sixteenth against a
 * quarter is the classic acid echo). Every repeat passes a 12 dB/octave high pass and a 12 dB/octave
 * low pass inside the feedback loop, so the echoes lose low end and top with each round: the repeats
 * never pile up under 140 Hz, where only kick and bass may play, and they darken like a tape echo
 * instead of ringing brighter than the source. A little of each side's output feeds the other side,
 * which spreads the tail.
 *
 * **Tempo.** The delay time in samples follows the tempo at every call of set(). Tempo ramps between
 * tracks move it by a fraction of a sample per chunk; the read position is interpolated linearly,
 * so the change neither clicks nor steps. A jump of the time setting glides over about 50 ms.
 * @author Rene Weller
 */
#pragma once
#include "phos/Dsp.h"
#include <vector>

namespace phos
{

/** @brief Stereo tempo delay. */
class TempoDelay
{
public:
    /**
     * @brief Allocates for a sample rate (up to two seconds of delay at any tempo the engine allows).
     * @param[in] sampleRate  the sample rate
     */
    void prepare( double sampleRate )
    {
        m_sr = sampleRate;
        size_t n = 1;
        while ( static_cast<double>( n ) < 2.5 * sampleRate ) n <<= 1;
        m_bufL.assign( n, 0.0f );
        m_bufR.assign( n, 0.0f );
        m_mask = n - 1;
        m_glide = static_cast<float>( 1.0 - std::exp( -1.0 / ( 0.05 * sampleRate ) ) );
        reset();
    }
    /** @brief Clears the lines and filters. */
    void reset()
    {
        std::fill( m_bufL.begin(), m_bufL.end(), 0.0f );
        std::fill( m_bufR.begin(), m_bufR.end(), 0.0f );
        m_write = 0;
        m_hpL.reset();
        m_hpR.reset();
        m_lpL.reset();
        m_lpR.reset();
        m_timeL = m_targetL;
        m_timeR = m_targetR;
    }
    /**
     * @brief Sets the delay.
     * @param[in] beatsLeft   delay times in beats
     * @param[in] beatsRight  delay times in beats
     * @param[in] bpm         current tempo
     * @param[in] feedback    0..0.9
     * @param[in] hpHz        band limits inside the loop
     * @param[in] lpHz        band limits inside the loop
     */
    void set( float beatsLeft, float beatsRight, double bpm, float feedback, float hpHz, float lpHz )
    {
        const double maxSamples = static_cast<double>( m_mask ) - 2.0;
        m_targetL = static_cast<float>( std::min( maxSamples, beatsLeft * 60.0 / bpm * m_sr ) );
        m_targetR = static_cast<float>( std::min( maxSamples, beatsRight * 60.0 / bpm * m_sr ) );
        if ( !m_primed )
        {
            m_timeL = m_targetL;
            m_timeR = m_targetR;
            m_primed = true;
        }
        m_feedback = clampv( feedback, 0.0f, 0.9f );
        const float sr = static_cast<float>( m_sr );
        m_hpL.setQ( std::max( hpHz, 150.0f ), 0.70710678f, sr );
        m_hpR.copyCoefficients( m_hpL );
        m_lpL.setQ( lpHz, 0.70710678f, sr );
        m_lpR.copyCoefficients( m_lpL );
    }
    /**
     * @brief Adds the delayed signal of a mono send to a stereo output.
     * @param[in]     send  mono input, @p n samples (already scaled by the send amount)
     * @param[in,out] L     outputs the echoes are added to
     * @param[in,out] R     outputs the echoes are added to
     * @param[in]     n     samples
     */
    void process( const float *send, float *L, float *R, int n )
    {
        // No idle shortcut: when a delay stops depending on where the host cuts its blocks, the output
        // would no longer be the same for every block size. Two lines and four filters are cheap.
        for ( int i = 0; i < n; ++i )
        {
            m_timeL += ( m_targetL - m_timeL ) * m_glide;
            m_timeR += ( m_targetR - m_timeR ) * m_glide;
            const float yl = read( m_bufL, m_timeL ), yr = read( m_bufR, m_timeR );
            float       lp, bp, hp;
            // Loop filters: high pass, then low pass, then the cross-feed.
            m_hpL.tick( yl, lp, bp, hp );
            const float fl = m_lpL.lp( hp );
            m_hpR.tick( yr, lp, bp, hp );
            const float fr = m_lpR.lp( hp );
            const float x = send[i];
            m_bufL[m_write] = x + m_feedback * ( 0.85f * fl + 0.15f * fr );
            m_bufR[m_write] = x + m_feedback * ( 0.85f * fr + 0.15f * fl );
            m_write = ( m_write + 1 ) & m_mask;
            L[i] += fl;
            R[i] += fr;
        }
    }

private:
    /**
     * @brief Reads @p buf @p delay samples back, linearly interpolated.
     * @param[in] buf    the delay buffer
     * @param[in] delay  the delay in samples
     * @return the sample read
     */
    float read( const std::vector<float> &buf, float delay ) const
    {
        // In double: at a write index of 2^17 a float resolves only 1/64 of a sample.
        const double pos = static_cast<double>( m_write ) - static_cast<double>( delay );
        const double fl = std::floor( pos );
        const float  frac = static_cast<float>( pos - fl );
        const size_t i0 = static_cast<size_t>( static_cast<long long>( fl ) ) & m_mask;
        const size_t i1 = ( i0 + 1 ) & m_mask;
        return buf[i0] + frac * ( buf[i1] - buf[i0] );
    }

    double             m_sr = 48000.0;        ///< sample rate
    std::vector<float> m_bufL;                ///< the left delay line, a power of two long
    std::vector<float> m_bufR;                ///< the right delay line
    size_t             m_mask = 0;            ///< the index mask
    size_t             m_write = 0;           ///< the write position
    float              m_timeL = 1000.0f;     ///< the left delay time now, samples
    float              m_timeR = 1000.0f;     ///< the right delay time now, samples
    float              m_targetL = 1000.0f;   ///< where the left one glides to
    float              m_targetR = 1000.0f;   ///< where the right one glides to
    float              m_glide = 0.001f;      ///< the glide coefficient
    float              m_feedback = 0.4f;     ///< feedback
    bool               m_primed = false;   ///< the times have been set once (the first set() jumps, later ones glide)
    Svf                m_hpL;              ///< the feedback path's high pass, left
    Svf                m_hpR;              ///< ... right
    Svf                m_lpL;              ///< the feedback path's low pass, left
    Svf                m_lpR;              ///< ... right
};

}   // namespace phos
